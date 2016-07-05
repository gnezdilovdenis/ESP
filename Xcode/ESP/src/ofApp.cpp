#include "ofApp.h"

#include <algorithm>
#include <math.h>

#include "user.h"

// If the feature output dimension is larger than 32, making the visualization a
// single output will be more visual.
const uint32_t kTooManyFeaturesThreshold = 32;

// This delay is needed so that UI can update to reflect the training status.
const uint32_t kDelayBeforeTraining = 50;  // milliseconds

// Instructions for each tab.
static const char* kCalibrateInstruction =
        "You must collect calibration samples before you can start training.\n"
        "Use key 1-9 to record calibration samples. "
        "Press `l` to load calibration data, `s` to save.";

static const char* kPipelineInstruction =
        "Press capital C/P/T/A to change tabs, `p` to pause or resume.\n";

static const char* kTrainingInstruction =
        "Press capital C/P/T/A to change tabs. "
        "`p` to pause or resume, 1-9 to record samples \n"
        "`r` to record test data, `f` to show features, `s` to save data"
        "`l` to load training data, and `t` to train a model.";

static const char* kAnalysisInstruction =
        "Press capital C/P/T/A to change tabs. \n"
        "Press `p` to pause or resume; hold `r` to record test data; "
        "press `s` to save test data and `l` to load test data.";

const double kPipelineHeightWeight = 0.3;
const ofColor kSerialSelectionColor = ofColor::fromHex(0x00FF00);

class Palette {
  public:
    vector<ofColor> generate(uint32_t n) {
        // TODO(benzh) fill instead of re-generate.
        if (n > size) {
            size = n;
            do_generate(size);
        }

        std::vector<ofColor> sliced(colors.begin(), colors.begin() + n);
        return sliced;
    }

    Palette() : size(256) {
        do_generate(size);
    }
  private:
    void do_generate(uint32_t n) {
        uint32_t numDimensions = n;
        // Code snippet from ofxGrtTimeseriesPlot.cpp

        colors.resize(n);
        // Setup the default colors
        if( numDimensions >= 1 ) colors[0] = ofColor(255,0,0); //red
        if( numDimensions >= 2 ) colors[1] = ofColor(0,255,0); //green
        if( numDimensions >= 3 ) colors[2] = ofColor(0,0,255); //blue
        //Randomize the remaining colors
        for(unsigned int n=3; n<numDimensions; n++){
            colors[n][0] = ofRandom(50,255);
            colors[n][1] = ofRandom(50,255);
            colors[n][2] = ofRandom(50,255);
        }
    }

    uint32_t size;
    std::vector<ofColor> colors;
};

void ofApp::useCalibrator(Calibrator &calibrator) {
    calibrator_ = &calibrator;
}

void ofApp::useIStream(IStream &stream) {
    if (!setup_finished_) istream_ = &stream;
}

void ofApp::usePipeline(GRT::GestureRecognitionPipeline &pipeline) {
    pipeline_ = &pipeline;
}

void ofApp::useOStream(OStream &stream) {
    if (!setup_finished_) ostreams_.push_back(&stream);
}

void ofApp::useOStream(OStreamVector &stream) {
    if (!setup_finished_) {
        ostreamvectors_.push_back(&stream);
    }
}

void ofApp::useTrainingSampleChecker(TrainingSampleChecker checker) {
    training_sample_checker_ = checker;
}

void ofApp::useTrainingDataAdvice(string advice) {
    training_data_advice_ = advice;
}

// TODO(benzh): initialize other members as well.
ofApp::ofApp() : fragment_(TRAINING),
                 num_pipeline_stages_(0),
                 calibrator_(nullptr),
                 training_data_manager_(kNumMaxLabels_),
                 should_save_calibration_data_(false),
                 should_save_training_data_(false),
                 should_save_test_data_(false),
                 is_training_scheduled_(false) {
}

//--------------------------------------------------------------
void ofApp::setup() {
    is_recording_ = false;

    // setup() is a user-defined function.
    ::setup(); setup_finished_ = true;

    for (OStream *ostream : ostreams_) {
        if (!(ostream->start())) {
            // TODO(benzh) If failed to start, alert in the GUI.
            ofLog(OF_LOG_ERROR) << "failed to connect to ostream";
        }
    }

    for (OStreamVector *ostream : ostreamvectors_) {
        if (!(ostream->start())) {
            // TODO(benzh) If failed to start, alert in the GUI.
            ofLog(OF_LOG_ERROR) << "failed to connect to ostream";
        }
    }

    if (calibrator_ && !(calibrator_->isCalibrated())) {
        fragment_ = CALIBRATION;
    } else {
        fragment_ = PIPELINE;
    }

    if (training_data_advice_ == "")
        training_data_advice_ = getTrainingDataAdvice();

    istream_->onDataReadyEvent(this, &ofApp::onDataIn);

    const vector<string>& istream_labels = istream_->getLabels();
    plot_raw_.setup(kBufferSize_, istream_->getNumOutputDimensions(), "Raw Data");
    plot_raw_.setDrawGrid(true);
    plot_raw_.setDrawInfoText(true);
    plot_raw_.setChannelNames(istream_labels);
    plot_inputs_.setup(kBufferSize_, istream_->getNumOutputDimensions(), "Input");
    plot_inputs_.setDrawGrid(true);
    plot_inputs_.setDrawInfoText(true);
    plot_inputs_.setChannelNames(istream_labels);
    plot_inputs_.onRangeSelected(this, &ofApp::onInputPlotSelection, NULL);

    plot_testdata_window_.setup(kBufferSize_, istream_->getNumOutputDimensions(), "Test Data");
    plot_testdata_window_.setDrawGrid(true);
    plot_testdata_window_.setDrawInfoText(true);

    plot_testdata_overview_.setup(istream_->getNumOutputDimensions(), "Overview");
    plot_testdata_overview_.onRangeSelected(this, &ofApp::onTestOverviewPlotSelection, NULL);

    Palette color_palette;

    // Parse the user supplied pipeline and extract information:
    //  o num_pipeline_stages_

    // 1. Parse pre-processing.
    uint32_t num_pre_processing = pipeline_->getNumPreProcessingModules();
    num_pipeline_stages_ += num_pre_processing;
    for (int i = 0; i < num_pre_processing; i++) {
        PreProcessing* pp = pipeline_->getPreProcessingModule(i);
        uint32_t dim = pp->getNumOutputDimensions();
        ofxGrtTimeseriesPlot plot;
        plot.setup(kBufferSize_, dim, "PreProcessing Stage " + std::to_string(i));
        plot.setDrawGrid(true);
        plot.setDrawInfoText(true);
        plot.setColorPalette(color_palette.generate(dim));
        plot_pre_processed_.push_back(plot);
    }

    // 2. Parse pre-processing.
    uint32_t num_feature_modules = pipeline_->getNumFeatureExtractionModules();
    uint32_t num_final_features = 0;
    for (int i = 0; i < num_feature_modules; i++) {
        vector<ofxGrtTimeseriesPlot> feature_at_stage_i;

        FeatureExtraction* fe = pipeline_->getFeatureExtractionModule(i);
        uint32_t feature_dim = fe->getNumOutputDimensions();

        if (feature_dim < kTooManyFeaturesThreshold) {
            for (int i = 0; i < feature_dim; i++) {
                ofxGrtTimeseriesPlot plot;
                plot.setup(kBufferSize_, 1, "Feature " + std::to_string(i));
                plot.setDrawInfoText(true);
                plot.setColorPalette(color_palette.generate(feature_dim));
                feature_at_stage_i.push_back(plot);
            }
            // Each feature will be draw with a height of stage_height *
            // kPipelineHeightWeight, therefore, the stage counts need to be
            // adjusted.
            num_pipeline_stages_ += ceil(feature_dim * kPipelineHeightWeight);
        } else {
            // We will have only one here.
            ofxGrtTimeseriesPlot plot;
            plot.setup(feature_dim, 1, "Feature");
            plot.setDrawGrid(true);
            plot.setDrawInfoText(true);
            plot.setColorPalette(color_palette.generate(feature_dim));
            feature_at_stage_i.push_back(plot);

            // Since we will be drawing each feature in a separate plot, count them
            // in pipeline stages.
            num_pipeline_stages_ += 1;
        }
        num_final_features = feature_dim;

        plot_features_.push_back(feature_at_stage_i);
    }

    for (uint32_t i = 0; i < num_final_features; i++) {
        sample_feature_ranges_.push_back(make_pair(0, 0));
    }

    if (calibrator_ != nullptr) {
        vector<CalibrateProcess>& calibrators = calibrator_->getCalibrateProcesses();
        for (uint32_t i = 0; i < calibrators.size(); i++) {
            uint32_t label_dim = istream_->getNumOutputDimensions();
            Plotter plot;
            plot.setup(label_dim, calibrators[i].getName(), calibrators[i].getDescription());
            plot.setColorPalette(color_palette.generate(label_dim));
            plot_calibrators_.push_back(plot);
        }
    }

    for (uint32_t i = 0; i < kNumMaxLabels_; i++) {
        uint32_t label_dim = istream_->getNumOutputDimensions();
        Plotter plot;
        plot.setup(label_dim, training_data_manager_.getLabelName(i + 1));
        plot.setColorPalette(color_palette.generate(label_dim));
        plot_samples_.push_back(plot);

        vector<Plotter> feature_plots;
        if (num_final_features < kTooManyFeaturesThreshold) {
            // For this label, `num_final_features` vertically stacked plots
            for (int j = 0; j < num_final_features; j++) {
                Plotter plot;
                plot.setup(1, "Feature " + std::to_string(j + 1));
                plot.setColorPalette(color_palette.generate(label_dim));
                feature_plots.push_back(plot);
            }
        } else {
            is_final_features_too_many_ = true;

            // The case of many features (like FFT), draw a single plot.
            Plotter plot;
            plot.setup(1, "Feature");
            plot.setColorPalette(color_palette.generate(label_dim));
            feature_plots.push_back(plot);
        }
        plot_sample_features_.push_back(feature_plots);

        plot_sample_indices_.push_back(-1);
        plot_sample_button_locations_.push_back(
            pair<ofRectangle, ofRectangle>(ofRectangle(), ofRectangle()));

        // =====================================================
        //  Add controls for each individual training classes
        // =====================================================
        TrainingSampleGuiListener *listener =
                new TrainingSampleGuiListener(this, i);

        ofxDatGui *gui = new ofxDatGui();
        gui->setWidth(80);
        gui->setAutoDraw(false);
        ofxDatGuiButton* rename_button = gui->addButton("rename");
        rename_button->onButtonEvent(
            listener, &TrainingSampleGuiListener::renameButtonPressed);
        rename_button->setStripeVisible(false);

        ofxDatGuiButton* delete_button = gui->addButton("delete");
        delete_button->onButtonEvent(
            listener, &TrainingSampleGuiListener::deleteButtonPressed);
        delete_button->setStripeVisible(false);

        ofxDatGuiButton* trim_button = gui->addButton("trim");
        trim_button->onButtonEvent(
            listener, &TrainingSampleGuiListener::trimButtonPressed);
        trim_button->setStripeVisible(false);

        ofxDatGuiButton* relabel_button = gui->addButton("relabel");
        relabel_button->onButtonEvent(
            listener, &TrainingSampleGuiListener::relabelButtonPressed);
        relabel_button->setStripeVisible(false);

        training_sample_guis_.push_back(gui);
    }

    for (uint32_t i = 0; i < plot_samples_.size(); i++) {
        plot_samples_[i].onRangeSelected(this, &ofApp::onPlotRangeSelected,
                                         reinterpret_cast<void*>(i + 1));
    }

    training_data_manager_.setNumDimensions(istream_->getNumOutputDimensions());
    predicted_label_ = 0;

    gui_.addHeader(":: Configuration ::");
    gui_.setAutoDraw(false);
    gui_.setPosition(ofGetWidth() - 300, 0);
    gui_.setWidth(280, 140);

    bool should_expand_gui = false;
    // Start input streaming.
    // If failed, this could be due to serial stream's port configuration.
    // We prompt to ask for the port.
    if (!istream_->start()) {
        if (ASCIISerialStream* ss = dynamic_cast<ASCIISerialStream*>(istream_)) {
            vector<string> serials = ss->getSerialDeviceList();
            serial_selection_dropdown_ =
                    gui_.addDropdown("Select A Serial Port", serials);
            serial_selection_dropdown_->onDropdownEvent(
                this, &ofApp::onSerialSelectionDropdownEvent);

            // Fine tune the theme (the default has a red color; we use
            // kSerialSelectionColor)
            ofxDatGuiTheme myTheme(true);
            myTheme.stripe.dropdown = kSerialSelectionColor;
            serial_selection_dropdown_->setTheme(&myTheme);

            gui_.addBreak()->setHeight(5.0f);

            status_text_ = "Please select a serial port from the dropdown menu";

            // We will keep the gui open.
            should_expand_gui = true;
        }
    }

    // Add the rest of the tuneables.
    for (Tuneable* t : tuneable_parameters_) {
        t->addToGUI(gui_);
    }

    // Two extra button for saving/loading tuneable parameters.
    gui_.addBreak()->setHeight(30.0f);
    ofxDatGuiButton* save_button = gui_.addButton("Save");
    ofxDatGuiButton* load_button = gui_.addButton("Load");
    save_button->onButtonEvent(this, &ofApp::saveTuneables);
    load_button->onButtonEvent(this, &ofApp::loadTuneables);

    gui_.addFooter();
    gui_.getFooter()->setLabelWhenExpanded("Click to apply and hide");
    gui_.getFooter()->setLabelWhenCollapsed("Click to open configuration");

    if (should_expand_gui) {
        gui_.expand();
    } else {
        gui_.collapse();
    }

    ofBackground(54, 54, 54);

    // Register myself as logging observer but disable first.
    GRT::ErrorLog::enableLogging(false);
    GRT::ErrorLog::registerObserver(*this);
}

void ofApp::onPlotRangeSelected(Plotter::CallbackArgs arg) {
    if (is_in_feature_view_) {
        uint32_t sample_index = reinterpret_cast<uint64_t>(arg.data) - 1;
        populateSampleFeatures(sample_index);
    }
}

void ofApp::populateSampleFeatures(uint32_t sample_index) {
    if (pipeline_->getNumFeatureExtractionModules() == 0) { return; }

    // Clean up historical data/caches.
    pipeline_->reset();

    vector<Plotter>& feature_plots = plot_sample_features_[sample_index];
    for (Plotter& plot : feature_plots) { plot.clearData(); }

    // 1. get samples
    MatrixDouble& sample = plot_samples_[sample_index].getData();
    uint32_t start = 0;
    uint32_t end = sample.getNumRows();
    if (is_final_features_too_many_) {
        pair<uint32_t, uint32_t> sel = plot_samples_[sample_index].getSelection();
        if (sel.second - sel.first > 10) {
            start = sel.first;
            end = sel.second;
        }
    }

    // 2. get features by flowing samples through
    for (uint32_t i = start; i < end; i++) {
        vector<double> data_point = sample.getRowVector(i);
        if (!pipeline_->preProcessData(data_point)) {
            ofLog(OF_LOG_ERROR) << "ERROR: Failed to compute features!";
            continue;
        }

        // Last stage of feature extraction.
        uint32_t j = pipeline_->getNumFeatureExtractionModules();
        vector<double> feature = pipeline_->getFeatureExtractionData(j - 1);

        for (uint32_t k = 0; k < feature_plots.size(); k++) {
            vector<double> feature_point = { feature[k] };
            feature_plots[k].push_back(feature_point);

            // sample_feature_ranges_[k].(first, second) tracks the min and max
            // for feature k so that the plots will be comparable.
            if (sample_feature_ranges_[k].first > feature[k]) {
                sample_feature_ranges_[k].first = feature[k];
            }
            if (sample_feature_ranges_[k].second < feature[k]) {
                sample_feature_ranges_[k].second = feature[k];
            }
        }

        if (is_final_features_too_many_) {
            assert(feature_plots.size() == 1);
            MatrixDouble feature_matrix;
            feature_matrix.resize(feature.size(), 1);
            feature_matrix.setColVector(feature, 0);
            sample_feature_ranges_[0].first = feature_matrix.getMinValue();
            sample_feature_ranges_[0].second = feature_matrix.getMaxValue();
            feature_plots[0].setData(feature_matrix);
        }
    }
}

void ofApp::onInputPlotSelection(InteractiveTimeSeriesPlot::CallbackArgs arg) {
    if (!enable_history_recording_) {
        plot_inputs_.clearSelection();
        return;
    }

    status_text_ = "Press 1-9 to extract from live data to training data.";
    is_in_history_recording_ = true;
    sample_data_.clear();
    sample_data_ = plot_inputs_.getSelectedData();
}

void ofApp::onTestOverviewPlotSelection(Plotter::CallbackArgs arg) {
    updateTestWindowPlot();
}

void ofApp::updateTestWindowPlot() {
    std::pair<uint32_t, uint32_t> sel = plot_testdata_overview_.getSelection();
    uint32_t start = 0;
    uint32_t end = test_data_.getNumRows();
    if (sel.second - sel.first > 10) {
        start = sel.first;
        end = sel.second;
    }
    plot_testdata_window_.reset();
    for (int i = start; i < end; i++) {
        plot_testdata_window_.setup(end - start, istream_->getNumInputDimensions(), "Test Data");
        for (int i = start; i < end; i++) {
            if (pipeline_->getTrained()) {
                int predicted_label = test_data_predicted_class_labels_[i];
                std::string title = training_data_manager_.getLabelName(predicted_label);
                plot_testdata_window_.update(test_data_.getRowVector(i), predicted_label != 0, title);
            } else {
                plot_testdata_window_.update(test_data_.getRowVector(i));
            }
        }
    }
}

void ofApp::runPredictionOnTestData() {
    test_data_predicted_class_labels_.resize(test_data_.getNumRows());
    for (int i = 0; i < test_data_.getNumRows(); i++) {
        if (pipeline_->getTrained()) {
            pipeline_->predict(test_data_.getRowVector(i));

            int predicted_label = pipeline_->getPredictedClassLabel();

            test_data_predicted_class_labels_[i] = predicted_label;
        } else {
            test_data_predicted_class_labels_[i] = 0;
        }
    }

}

void ofApp::saveCalibrationData() {
    ofFileDialogResult result = ofSystemSaveDialog("CalibrationData.grt",
                                                   "Save your calibration data?");
    if (result.bSuccess) {
        // Pack calibration samples into a TimeSeriesClassificationData so they
        // can all be saved in a single file.
        GRT::TimeSeriesClassificationData data(istream_->getNumOutputDimensions(),
                                               "CalibrationData");
        auto calibrators = calibrator_->getCalibrateProcesses();
        for (int i = 0; i < calibrators.size(); i++) {
            data.addSample(i, calibrators[i].getData());
            data.setClassNameForCorrespondingClassLabel(calibrators[i].getName(), i);
        }
        data.save(result.getPath());
    }

    should_save_calibration_data_ = false;
}

void ofApp::loadCalibrationData() {
    vector<CalibrateProcess>& calibrators = calibrator_->getCalibrateProcesses();
    GRT::TimeSeriesClassificationData data;
    ofFileDialogResult result = ofSystemLoadDialog("Load existing calibration data", true);

    if (!result.bSuccess) return;

    if (!data.load(result.getPath()) ){
        ofLog(OF_LOG_ERROR) << "Failed to load the calibration data!"
                            << " path: " << result.getPath();
        return;
    }

    if (data.getNumSamples() != calibrators.size()) {
        ofLog(OF_LOG_ERROR) << "Number of samples in file differs from the "
                            << "number of calibration samples.";
        return;
    }

    if (data.getNumDimensions() != istream_->getNumOutputDimensions()) {
        ofLog(OF_LOG_ERROR) << "Number of dimensions of data in file differs "
                            << "from the number of dimensions expected.";
        return;
    }

    for (int i = 0; i < data.getNumSamples(); i++) {
        if (data.getClassNameForCorrespondingClassLabel(i) != calibrators[i].getName()) {
            ofLog(OF_LOG_WARNING) << "Name of saved calibration sample " << (i + 1) << " ('"
                                  << data.getClassNameForCorrespondingClassLabel(i)
                                  << "') differs from current calibration sample name ('"
                                  << calibrators[i].getName() << "')";
        }
        plot_calibrators_[i].setData(data[i].getData());
        calibrators[i].setData(data[i].getData());
        calibrators[i].calibrate();
    }

    plot_inputs_.reset();
    should_save_calibration_data_ = false;
}

void ofApp::saveTuneables(ofxDatGuiButtonEvent e) {
    ofFileDialogResult result = ofSystemSaveDialog("TuneableParameters.grt",
                                                   "Save your tuneable parameters?");
    if (!result.bSuccess) { return; }

    std::ofstream file(result.getPath());
    for (Tuneable* t : tuneable_parameters_) {
        file << t->toString() << std::endl;
    }
    file.close();
}

void ofApp::loadTuneables(ofxDatGuiButtonEvent e) {
    ofFileDialogResult result = ofSystemLoadDialog("Save tuneable parameters", true);
    if (!result.bSuccess) { return; }

    std::string line;
    std::ifstream file(result.getPath());
    for (Tuneable* t : tuneable_parameters_) {
        std::getline(file, line);
        t->fromString(line);
    }
    file.close();
}

void ofApp::onSerialSelectionDropdownEvent(ofxDatGuiDropdownEvent e) {
    if (istream_->hasStarted()) { return; }

    if (ASCIISerialStream* ss = dynamic_cast<ASCIISerialStream*>(istream_)) {
        if (ss->selectSerialDevice(e.child)) {
            serial_selection_dropdown_->collapse();
            serial_selection_dropdown_->setVisible(false);
            gui_.collapse();
            status_text_ = "";
        } else {
            status_text_ = "Please select another serial port!";
        }
    }
}

void ofApp::renameTrainingSample(int num) {
    // If we are already in renaming, finish it by calling rename...Done.
    if (is_in_renaming_) {
        renameTrainingSampleDone();
    }

    int label = num + 1;
    // TODO(benzh) This should be renaming each sample, instead of each label.
    // Currently, we are in the transition from managing everything in ofApp to
    // individual components (such as TrainingDataManager).
    rename_title_ = training_data_manager_.getLabelName(label);

    is_in_renaming_ = true;
    rename_target_ = label;
    display_title_ = rename_title_;
    plot_samples_[rename_target_ - 1].renameTitleStart();
    plot_samples_[rename_target_ - 1].setTitle(display_title_);
    ofAddListener(ofEvents().update, this, &ofApp::updateEventReceived);
}

void ofApp::renameTrainingSampleDone() {
    training_data_manager_.setNameForLabel(rename_title_, rename_target_);

    is_in_renaming_ = false;
    plot_samples_[rename_target_ - 1].setTitle(rename_title_);
    plot_samples_[rename_target_ - 1].renameTitleDone();
    ofRemoveListener(ofEvents().update, this, &ofApp::updateEventReceived);
    should_save_training_data_ = true;
}

void ofApp::updateEventReceived(ofEventArgs& arg) {
    update_counter_++;

    // Assuming 60fps, to update the cursor every 0.1 seconds
    int period = 60 * 0.1;
    if (is_in_renaming_) {
        if (update_counter_ == period) {
            display_title_ = rename_title_ + "_";
        } else if (update_counter_ == period * 2) {
            display_title_ = rename_title_;
            update_counter_ = 0;
        }
        plot_samples_[rename_target_ - 1].setTitle(display_title_);
    }
}

void ofApp::deleteTrainingSample(int num) {
    int label = num + 1;

    if (plot_sample_indices_[num] < 0) { return; }
    training_data_manager_.deleteSample(label, plot_sample_indices_[num]);

    uint32_t num_sample_left = training_data_manager_.getNumSampleForLabel(label);

    // Before, we might be showing the last one; adjust the sample down by one
    if (plot_sample_indices_[num] == num_sample_left) {
        plot_sample_indices_[num]--;
    }
    if (plot_sample_indices_[num] >= 0) {
        plot_samples_[num].setData(
            training_data_manager_.getSample(label, plot_sample_indices_[num]));
    } else {
        plot_samples_[num].reset();
        plot_sample_indices_[num] = -1;
    }

    populateSampleFeatures(num);
    should_save_training_data_ = true;
}

void ofApp::trimTrainingSample(int num) {
    pair<uint32_t, uint32_t> selection = plot_samples_[num].getSelection();

    // Return if no selection or the range is too small (if user left clicked).
    if (selection.second - selection.first < 10) { return; }

    int label = num + 1;

    training_data_manager_.trimSample(label, plot_sample_indices_[num],
                                      selection.first, selection.second);
    plot_samples_[num].setData(
        training_data_manager_.getSample(label, plot_sample_indices_[num]));

    populateSampleFeatures(num);
    should_save_training_data_ = true;
}

void ofApp::relabelTrainingSample(int num) {
    // After this button is pressed, we enter relabel_mode
    is_in_relabeling_ = true;
    relabel_source_ = num + 1;
}

void ofApp::doRelabelTrainingSample(uint32_t source, uint32_t target) {
    if (source == target) {
        return;
    }

    // // plot_samples_ (num) is 0-based, labels (source and target) are 1-based.
    uint32_t num = source - 1;
    uint32_t label = source;
    if (plot_sample_indices_[num] < 0) { return; }
    training_data_manager_.relabelSample(source, plot_sample_indices_[num], target);

    // Update the source plot
    uint32_t num_source_sample_left = training_data_manager_.getNumSampleForLabel(source);
    if (plot_sample_indices_[num] == num_source_sample_left) {
        plot_sample_indices_[num]--;
    }
    if (plot_sample_indices_[num] >= 0) {
        plot_samples_[num].setData(
            training_data_manager_.getSample(source, plot_sample_indices_[num]));
    } else {
        plot_samples_[num].reset();
        plot_sample_indices_[num] = -1;
    }
    populateSampleFeatures(num);

    // Update the target plot
    plot_sample_indices_[target - 1]++;
    plot_samples_[target - 1].setData(
        training_data_manager_.getSample(target, plot_sample_indices_[target - 1]));
    populateSampleFeatures(target - 1);

    should_save_training_data_ = true;
}

string ofApp::getTrainingDataAdvice() {
    if (!pipeline_->getIsClassifierSet()) return "";
    if (dynamic_cast<DTW *>(pipeline_->getClassifier())) {
        return "This algorithm looks for the closest training sample. "
            "As a result, you don't need a lot of training data but any "
            "individual bad training sample can cause problems.";
    }
    if (dynamic_cast<ANBC *>(pipeline_->getClassifier())) {
        return "This algorithm uses an average of the training data. "
            "As a result, recording additional training data can help the "
            "performance of the algorithm. For each class, try to record "
            "training data that represents the range of situations you want "
            "to be recognized.";
    }
    if (dynamic_cast<SVM *>(pipeline_->getClassifier())) {
        return "This algorithm looks at the boundaries between the different "
            "classes of training data. As a result, it can help to record "
            "additional data at the boundaries between the different classes "
            "you want to recognize.";
    }
    return "";
}

//--------------------------------------------------------------
void ofApp::update() {
    std::lock_guard<std::mutex> guard(input_data_mutex_);
    for (int i = 0; i < input_data_.getNumRows(); i++){
        vector<double> raw_data = input_data_.getRowVector(i);
        vector<double> data_point;
        plot_raw_.update(raw_data);
        if (calibrator_ == nullptr) {
            data_point = raw_data;
        } else if (calibrator_->isCalibrated()) {
            data_point = calibrator_->calibrate(raw_data);
        } else {
            // Not calibrated! For now, force the tab to be CALIBRATION.
            fragment_ = CALIBRATION;
        }

        std::string title;

        if (pipeline_->getTrained()) {
            pipeline_->predict(data_point);
            predicted_label_ = pipeline_->getPredictedClassLabel();
            predicted_class_distances_ = pipeline_->getClassDistances();
            predicted_class_likelihoods_ = pipeline_->getClassLikelihoods();
            predicted_class_labels_ = pipeline_->getClassifier()->getClassLabels();

            if (predicted_label_ != 0) {
                for (OStream *ostream : ostreams_)
                    ostream->onReceive(predicted_label_);
                for (OStream *ostream : ostreamvectors_)
                    ostream->onReceive(predicted_label_);

                title = training_data_manager_.getLabelName(predicted_label_);
            }
        }

        plot_inputs_.update(data_point, predicted_label_ != 0, title);

        if (istream_->hasStarted()) {
            if (!pipeline_->preProcessData(data_point)) {
                ofLog(OF_LOG_ERROR) << "ERROR: Failed to compute features!";
            }

            vector<double> data = data_point;

            for (int j = 0; j < pipeline_->getNumPreProcessingModules(); j++) {
                data = pipeline_->getPreProcessedData(j);
                plot_pre_processed_[j].update(data);
            }

            for (int j = 0; j < pipeline_->getNumFeatureExtractionModules(); j++) {
                // Working on j-th stage.
                data = pipeline_->getFeatureExtractionData(j);
                if (data.size() < kTooManyFeaturesThreshold) {
                    for (int k = 0; k < data.size(); k++) {
                        vector<double> v = { data[k] };
                        plot_features_[j][k].update(v);
                    }
                } else {
                    assert(plot_features_[j].size() == 1);
                    plot_features_[j][0].setData(data);
                }
            }

            // If there's no classifier set, we've got a signal processing
            // pipeline and we should send the results of the pipeline to
            // any OStreamVector instances that are listening for it.
            // TODO(damellis): this logic will need updating when / if we
            // support regression and clustering pipelines.
            if (!pipeline_->getIsClassifierSet()) {
                for (OStreamVector *stream : ostreamvectors_) {
                    stream->onReceive(data);
                }
            }
        }

        if (is_recording_) {
            if (fragment_ == CALIBRATION) {
                sample_data_.push_back(raw_data);
            } else {
                sample_data_.push_back(data_point);
            }
        }
    }

    if (is_training_scheduled_ == true &&
        (ofGetElapsedTimeMillis() - schedule_time_ > kDelayBeforeTraining)) {
        trainModel();
    }
}

void ofDrawColoredBitmapString(ofColor color,
                               const string& text,
                               float x, float y) {
    ofPushStyle();
    ofSetColor(color);
    ofDrawBitmapString(text, x, y);
    ofPopStyle();
}

//--------------------------------------------------------------
void ofApp::draw() {
    // Hacky panel on the top.
    const uint32_t left_margin = 10;
    const uint32_t top_margin = 20;
    const uint32_t margin = 20;

    if (pipeline_->getClassifier() != nullptr) {
        ofDrawBitmapString("[C]alibration\t[P]ipeline\t[A]nalysis\t[T]raining",
                           left_margin, top_margin);
    } else {
        ofDrawBitmapString("[C]alibration\t[P]ipeline\t[A]nalysis",
                           left_margin, top_margin);
    }

    ofColor red = ofColor(0xFF, 0, 0);
    uint32_t tab_start = 0;
    uint32_t kTabWidth = 120;

    switch (fragment_) {
        case CALIBRATION:
            ofDrawColoredBitmapString(red, "[C]alibration\t",
                                      left_margin, top_margin);
            ofDrawBitmapString(kCalibrateInstruction,
                               left_margin, top_margin + margin);
            drawCalibration();
            break;
        case PIPELINE:
            ofDrawColoredBitmapString(red, "\t\t[P]ipeline\t",
                                      left_margin, top_margin);
            ofDrawBitmapString(kPipelineInstruction,
                               left_margin, top_margin + margin);
            drawLivePipeline();
            tab_start += kTabWidth;
            break;
        case ANALYSIS:
            ofDrawColoredBitmapString(red, "\t\t\t\t[A]nalysis",
                                      left_margin, top_margin);
            ofDrawBitmapString(kAnalysisInstruction,
                               left_margin, top_margin + margin);
            drawAnalysis();
            tab_start += 2 * kTabWidth;
            break;
        case TRAINING:
            if (pipeline_->getClassifier() == nullptr) { break; }
            ofDrawColoredBitmapString(red, "\t\t\t\t\t\t[T]raining",
                                      left_margin, top_margin);
            ofDrawBitmapString(kTrainingInstruction,
                               left_margin, top_margin + margin);
            drawTrainingInfo();
            tab_start += 3 * kTabWidth;
            break;
        default:
            ofLog(OF_LOG_ERROR) << "Unknown tag!";
            break;
    }

    // Draw a shape like the following to indicate a tab.
    //          ______
    // ________|     |____________
    uint32_t bottom = top_margin + 5;
    uint32_t ceiling = 5;
    ofDrawLine(0, bottom, tab_start, bottom);
    ofDrawLine(tab_start, bottom, tab_start, ceiling);
    ofDrawLine(tab_start, ceiling, tab_start + kTabWidth, ceiling);
    ofDrawLine(tab_start + kTabWidth, ceiling, tab_start + kTabWidth, bottom);
    ofDrawLine(tab_start + kTabWidth, bottom, ofGetWidth(), bottom);

    // Status text at the bottom
    ofDrawBitmapString(status_text_, left_margin, ofGetHeight() - 20);

    gui_.draw();
}

void ofApp::drawCalibration() {
    uint32_t margin = 30;
    uint32_t stage_left = 10;
    uint32_t stage_top = 70;
    uint32_t stage_height = (ofGetHeight() - stage_top - margin * 3) / 2;
    uint32_t stage_width = ofGetWidth() - margin;

    // 1. Draw Input.
    ofPushStyle();
    plot_raw_.draw(stage_left, stage_top, stage_width, stage_height);
    ofPopStyle();
    stage_top += stage_height + margin;

    if (plot_calibrators_.size() == 0) return;

    float minY = plot_raw_.getRanges().first;
    float maxY = plot_raw_.getRanges().second;

    // 2. Draw Calibrators.
    int width = stage_width / plot_calibrators_.size();
    for (int i = 0; i < plot_calibrators_.size(); i++) {
        int x = stage_left + width * i;
        ofPushStyle();
        plot_calibrators_[i].setRanges(minY, maxY);
        plot_calibrators_[i].draw(x, stage_top, width, stage_height);
        ofPopStyle();
    }
}

void ofApp::drawLivePipeline() {
    // A Pipeline was parsed in the ofApp::setup function and here we simple
    // draw the pipeline information.
    uint32_t margin = 30;
    uint32_t stage_left = 10;
    uint32_t stage_top = 70;
    uint32_t stage_height = // Hacky math for dimensions.
            (ofGetHeight() - margin - stage_top) / (num_pipeline_stages_ + 1) - margin;
    uint32_t stage_width = ofGetWidth() - margin;

    // 1. Draw Input.
    ofPushStyle();
    plot_inputs_.draw(stage_left, stage_top, stage_width, stage_height);
    ofPopStyle();
    stage_top += stage_height + margin;

    // 2. Draw pre-processing: iterate all stages.
    for (int i = 0; i < pipeline_->getNumPreProcessingModules(); i++) {
        // working on pre-processing stage i.
        ofPushStyle();
        plot_pre_processed_[i].
                draw(stage_left, stage_top, stage_width, stage_height);
        ofPopStyle();
        stage_top += stage_height + margin;
    }

    // 3. Draw features.
    for (int i = 0; i < pipeline_->getNumFeatureExtractionModules(); i++) {
        // working on feature extraction stage i.
        ofPushStyle();
        uint32_t height = plot_features_[i].size() == 1 ?
                stage_height : stage_height * kPipelineHeightWeight;
        for (int j = 0; j < plot_features_[i].size(); j++) {
            plot_features_[i][j].draw(stage_left, stage_top, stage_width, height);
            stage_top += height;
        }
        ofPopStyle();
        stage_top += margin;
    }
}

void ofApp::drawTrainingInfo() {
    uint32_t margin_left = 10;
    uint32_t margin_top = 70;
    uint32_t margin = 30;
    uint32_t stage_left = margin_left;
    uint32_t stage_top = margin_top;
    uint32_t stage_width = ofGetWidth() - margin;
    uint32_t stage_height = (ofGetHeight() - 200 - 4 * margin) / 2;

    // 1. Draw Input
    if (!is_in_feature_view_) {
        ofPushStyle();
        plot_inputs_.draw(stage_left, stage_top, stage_width, stage_height);
        ofPopStyle();
        stage_top += stage_height + margin;
    }

    // 2. Draw advice for training data (if any)
    if (training_data_advice_ != "") {
        ofxParagraph paragraph(training_data_advice_, stage_width);
        paragraph.setFont("ofxbraitsch/fonts/Verdana.ttf", 11);
        paragraph.setColor(0xffffff);
        paragraph.setIndent(0);
        paragraph.setLeading(0);
        paragraph.draw(stage_left, stage_top);
        stage_top += paragraph.getHeight();
    }

    // 3. Draw samples
    // Currently we support kNumMaxLabels_ labels
    uint32_t width = stage_width / kNumMaxLabels_;
    float minY = plot_inputs_.getRanges().first;
    float maxY = plot_inputs_.getRanges().second;

    for (uint32_t i = 0; i < kNumMaxLabels_; i++) {
        uint32_t label = i + 1;
        uint32_t x = stage_left + i * width;
        plot_samples_[i].setRanges(minY, maxY, true);
        plot_samples_[i].draw(x, stage_top, width, stage_height);

        uint32_t num_samples = training_data_manager_.getNumSampleForLabel(label);
        ofDrawBitmapString(
            std::to_string(plot_sample_indices_[i] + 1) + " / " +
            std::to_string(training_data_manager_.getNumSampleForLabel(label)),
            x + width / 2 - 20,
            stage_top + stage_height + 20);
        if (plot_sample_indices_[i] > 0) {
            ofDrawBitmapString("<-", x, stage_top + stage_height + 20);
        }
        if (plot_sample_indices_[i] + 1 < num_samples) {
            ofDrawBitmapString("->", x + width - 20, stage_top + stage_height + 20);
        }
        plot_sample_button_locations_[i].first.set(x, stage_top + stage_height, 20, 20);
        plot_sample_button_locations_[i].second.set(x + width - 20, stage_top + stage_height, 20, 20);

        // TODO(dmellis): only update these values when the screen size changes.
        training_sample_guis_[i]->setPosition(x + margin / 8,
                                              stage_top + stage_height + 30);
        training_sample_guis_[i]->setWidth(width - margin / 4);
        training_sample_guis_[i]->draw();
    }

    stage_top += stage_height + 30 + training_sample_guis_[0]->getHeight();
    for (int i = 0; i < predicted_class_distances_.size() &&
                 i < predicted_class_likelihoods_.size(); i++) {
        ofColor backgroundColor, textColor;
        UINT label = predicted_class_labels_[i];
        if (predicted_label_ == label) {
            backgroundColor = ofColor(255);
            textColor = ofColor(0);
        } else {
            backgroundColor = ofGetBackgroundColor();
            textColor = ofColor(255);
        }
        ofDrawBitmapStringHighlight(
            std::to_string(predicted_class_distances_[i]).substr(0, 6),
            stage_left + (label - 1) * width,
            stage_top + margin,
            backgroundColor, textColor);
        ofDrawBitmapStringHighlight(
            std::to_string(predicted_class_likelihoods_[i]).substr(0, 6),
            stage_left + (label - 1) * width,
            stage_top + margin * 3 / 2,
            backgroundColor, textColor);
    }

    if (!is_in_feature_view_) { return; }
    if (pipeline_->getNumFeatureExtractionModules() == 0) { return; }
    // 3. Features
    stage_top += margin * 2;
    for (uint32_t i = 0; i < kNumMaxLabels_; i++) {
        uint32_t x = stage_left + i * width;
        uint32_t y = stage_top;
        vector<Plotter> feature_plots = plot_sample_features_[i];
        uint32_t margin = 5;
        uint32_t height = stage_height / feature_plots.size() - margin;

        for (uint32_t j = 0; j < feature_plots.size(); j++) {
            pair<double, double> range = sample_feature_ranges_[j];

            feature_plots[j].setRanges(range.first, range.second);
            feature_plots[j].draw(x, y, width, height);
            y += height + margin;
        }
    }
}

void ofApp::drawAnalysis() {
    uint32_t margin_left = 10;
    uint32_t margin_top = 70;
    uint32_t margin = 30;
    uint32_t stage_left = margin_left;
    uint32_t stage_top = margin_top;
    uint32_t stage_width = ofGetWidth() - margin;
    uint32_t stage_height = (ofGetHeight() - 4 * margin - margin_top) / 2.25;

    // 1. Draw Input
    ofPushStyle();
    plot_inputs_.draw(stage_left, stage_top, stage_width, stage_height);
    ofPopStyle();
    stage_top += stage_height + margin;

    ofPushStyle();
    plot_testdata_window_.draw(stage_left, stage_top, stage_width, stage_height);
    ofPopStyle();
    stage_top += stage_height + margin;

    ofPushStyle();
    plot_testdata_overview_.draw(stage_left, stage_top, stage_width, stage_height / 4);
    ofPopStyle();
}

void ofApp::exit() {
    if (training_thread_.joinable()) {
        training_thread_.join();
    }
    istream_->stop();

    // Save data here!
    if (should_save_calibration_data_) { saveCalibrationData(); }
    if (should_save_training_data_) { saveTrainingData(); }
    if (should_save_test_data_) { saveTestData(); }
}

void ofApp::saveTrainingData() {
    ofFileDialogResult result = ofSystemSaveDialog("TrainingData.grt",
                                                   "Save your training data?");
    if (result.bSuccess) {
        training_data_manager_.save(result.getPath());
    }

    should_save_training_data_ = false;
}

void ofApp::saveTestData() {
    ofFileDialogResult result = ofSystemSaveDialog("TestData.csv",
                                                   "Save your test data?");
    if (result.bSuccess) {
        test_data_.save(result.getPath());
    }

    should_save_test_data_ = false;
}

void ofApp::onDataIn(GRT::MatrixDouble input) {
    std::lock_guard<std::mutex> guard(input_data_mutex_);
    input_data_ = input;
}

//--------------------------------------------------------------
void ofApp::toggleFeatureView() {
    if (fragment_ != TRAINING) { return; }

    if (is_in_feature_view_) {
        is_in_feature_view_ = false;
    } else {
        is_in_feature_view_ = true;
        for (uint32_t i = 0; i < kNumMaxLabels_; i++) {
            populateSampleFeatures(i);
        }
    }
}

void ofApp::beginTrainModel() {
    // Update UI to reflect training starts.
    status_text_ = "Training the model . . .";
    is_training_scheduled_ = true;
    schedule_time_ = ofGetElapsedTimeMillis();
}

void ofApp::trainModel() {
    is_training_scheduled_ = false;

   // If prior training has not finished, we wait.
   if (training_thread_.joinable()) {
       training_thread_.join();
   }

   auto training_func = [this]() -> bool {
       ofLog() << "Training started";
       bool training_status = false;

       // Enable logging. GRT error logs will call ofApp::notify().
       GRT::ErrorLog::enableLogging(true);

       if (pipeline_->train(training_data_manager_.getAllData())) {
           ofLog() << "Training is successful";

           for (Plotter& plot : plot_samples_) {
               assert(true == plot.clearContentModifiedFlag());
           }
           
           scoreTrainingData();

           training_status = true;
       } else {
           ofLog(OF_LOG_ERROR) << "Failed to train the model";
       }

       // Stop logging.
       GRT::ErrorLog::enableLogging(false);
       return training_status;
   };

   // TODO(benzh) Fix data race issue later.
   if (training_func()) {
       fragment_ = TRAINING;
       runPredictionOnTestData();
       updateTestWindowPlot();
       pipeline_->reset();

       status_text_ = "Training was successful";
   }
}

void ofApp::scoreTrainingData() {
    TimeSeriesClassificationData training_data = training_data_manager_.getAllData();
    for (int i = 0; i < training_data.getNumSamples(); i++) {
        TimeSeriesClassificationSample sample = training_data[i];
        ofLog(OF_LOG_NOTICE) << "sample " << i << " (class " << sample.getClassLabel() << "):";
        vector<double> likelihoods(pipeline_->getNumClasses(), 0.0);
        for (int j = 0; j < sample.getData().getNumRows(); j++) {
            pipeline_->predict(sample.getData().getRowVector(j));
            auto l = pipeline_->getClassLikelihoods();
            for (int j = 0; j < likelihoods.size(); j++) likelihoods[j] += l[j];
        }
        for (int j = 0; j < likelihoods.size(); j++) {
            ofLog(OF_LOG_NOTICE) << "\t" << (j + 1) << ": " << likelihoods[j] << "%";
        }
        pipeline_->reset();
    }
}

void ofApp::loadTrainingData() {
    GRT::TimeSeriesClassificationData training_data;
    ofFileDialogResult result = ofSystemLoadDialog("Load existing data", true);

    if (!result.bSuccess) return;

    if (!training_data_manager_.load(result.getPath())) {
        ofLog(OF_LOG_ERROR) << "Failed to load the training data!"
                            << " path: " << result.getPath();
    }

    for (uint32_t i = 0; i < kNumMaxLabels_; i++) {
        uint32_t num = training_data_manager_.getNumSampleForLabel(i);
        plot_sample_indices_[i] = num;

        plot_samples_[i].setData(training_data_manager_.getSample(i, num - 1));
        std::string title = training_data_manager_.getLabelName(i);
        plot_samples_[i].setTitle(title);
    }

    // After we load the training data,
    should_save_training_data_ = false;

    beginTrainModel();
}

void ofApp::loadTestData() {
    GRT::MatrixDouble test_data;
    ofFileDialogResult result = ofSystemLoadDialog("Load existing test data", true);

    if (!result.bSuccess) return;

    if (!test_data.load(result.getPath()) ){
        ofLog(OF_LOG_ERROR) << "Failed to load the test data!"
                            << " path: " << result.getPath();
    }

    test_data_ = test_data;
    should_save_test_data_ = false;
    plot_testdata_overview_.setData(test_data_);
    runPredictionOnTestData();
    updateTestWindowPlot();
}

void ofApp::reloadPipelineModules() {
    pipeline_->clearAll();
    ::setup();
}

//--------------------------------------------------------------
void ofApp::keyPressed(int key){
    if (is_in_renaming_) {
        // Add normal characters.
        if (key >= 32 && key <= 126) {
            // key code 32 is for space, we remap it to '_'.
            key = (key == 32) ? '_' : key;
            rename_title_ += key;
            return;
        }

        switch (key) {
          case OF_KEY_BACKSPACE:
            rename_title_ = rename_title_.substr(0, rename_title_.size() - 1);
            break;
          case OF_KEY_RETURN:
            renameTrainingSampleDone();
            return;
          default:
            break;
        }

        plot_samples_[rename_target_ - 1].setTitle(display_title_);
        return;
    }

    if (is_in_history_recording_) { return; }

    // If in relabeling, take action at key release stage.
    if (is_in_relabeling_) { return; }

    if (key >= '1' && key <= '9') {
        if (!is_recording_) {
            is_recording_ = true;
            label_ = key - '0';
            sample_data_.clear();
        }
    }

    switch (key) {
        case 'r':
            if (!is_recording_) {
                is_recording_ = true;
                label_ = 255;
                sample_data_.clear();
                test_data_.clear();
                plot_testdata_window_.reset();
            }
            break;
        case 'f': toggleFeatureView(); break;
        case 'l':
            if (fragment_ == CALIBRATION) loadCalibrationData();
            else if (fragment_ == TRAINING) loadTrainingData();
            else if (fragment_ == ANALYSIS) loadTestData();
            break;
        case 'p': {
            istream_->toggle();
            enable_history_recording_ = !enable_history_recording_;
            input_data_.clear();
            break;
        }
        case 's':
            if (fragment_ == CALIBRATION) saveCalibrationData();
            else if (fragment_ == TRAINING) saveTrainingData();
            else if (fragment_ == ANALYSIS) saveTestData();
            break;
        case 't': beginTrainModel(); break;

        // Tab related
        case 'C': fragment_ = CALIBRATION; break;
        case 'P': fragment_ = PIPELINE; break;
        case 'T': {
            if (pipeline_->getClassifier() != nullptr) {
                fragment_ = TRAINING;
            }
            break;
        }
        case 'A': fragment_ = ANALYSIS; break;
    }
}

void ofApp::keyReleased(int key) {
    if (is_in_renaming_) { return; }
    if (is_in_history_recording_) {
        // Pressing 1-9 will turn the samples into training data
        if (key >= '1' && key <= '9') {
            label_ = key - '0';
            training_data_manager_.addSample(key - '0', sample_data_);
            int num_samples = training_data_manager_.getNumSampleForLabel(label_);

            plot_samples_[label_ - 1].setData(sample_data_);
            plot_sample_indices_[label_ - 1] = num_samples - 1;

            should_save_training_data_ = true;
        }
        // Reset the status of the GUI
        is_in_history_recording_ = false;
        status_text_ = "";
        plot_inputs_.clearSelection();
        return;
    }

    if (is_in_relabeling_ && key >= '1' && key <= '9') {
        doRelabelTrainingSample(relabel_source_, key - '0');
        is_in_relabeling_ = false;
        return;
    }

    is_recording_ = false;
    if (key >= '1' && key <= '9') {
        if (fragment_ == CALIBRATION) {
            if (calibrator_ == nullptr) { return; }

            vector<CalibrateProcess>& calibrators = calibrator_->getCalibrateProcesses();
            if (label_ - 1 < calibrators.size()) {
                plot_calibrators_[label_ - 1].setData(sample_data_);
                calibrators[label_ - 1].setData(sample_data_);

                CalibrateResult result = calibrators[label_ - 1].calibrate();
                if (result.getResult() == CalibrateResult::SUCCESS) {
                    plot_inputs_.reset();
                    should_save_calibration_data_ = true;
                }

                status_text_ = calibrators[label_ - 1].getName() +
                        " calibration: " +
                        result.getMessage();
            }
        } else if (fragment_ == TRAINING) {
            if (training_sample_checker_) {
                TrainingSampleCheckerResult result =
                    training_sample_checker_(sample_data_);
                status_text_ = plot_samples_[label_ - 1].getTitle() +
                    " check: " + result.getMessage();

                // Don't save sample if the checker returns failure.
                if (result.getResult() == TrainingSampleCheckerResult::FAILURE)
                    return;
            }

            training_data_manager_.addSample(label_, sample_data_);
            int num_samples = training_data_manager_.getNumSampleForLabel(label_);

            plot_samples_[label_ - 1].setData(sample_data_);
            plot_sample_indices_[label_ - 1] = num_samples - 1;

            should_save_training_data_ = true;
        }
    }

    if (key == 'r') {
        test_data_ = sample_data_;
        plot_testdata_overview_.setData(test_data_);
        runPredictionOnTestData();
        updateTestWindowPlot();
        should_save_test_data_ = true;
    }
}

//--------------------------------------------------------------
void ofApp::mouseMoved(int x, int y ) {

}

//--------------------------------------------------------------
void ofApp::mouseDragged(int x, int y, int button) {

}

//--------------------------------------------------------------
void ofApp::mousePressed(int x, int y, int button) {

}

//--------------------------------------------------------------
void ofApp::mouseReleased(int x, int y, int button) {
    // Navigating between samples (samples themselves are not changed).
    for (int i = 0; i < kNumMaxLabels_; i++) {
        int label = i + 1;
        if (plot_sample_button_locations_[i].first.inside(x, y)) {
            if (plot_sample_indices_[i] > 0) {
                plot_sample_indices_[i]--;
                plot_samples_[i].setData(
                    training_data_manager_.getSample(label, plot_sample_indices_[i]));
                assert(true == plot_samples_[i].clearContentModifiedFlag());
                populateSampleFeatures(i);
            }
        }
        if (plot_sample_button_locations_[i].second.inside(x, y)) {
            if (plot_sample_indices_[i] + 1 < training_data_manager_.getNumSampleForLabel(label)) {
                plot_sample_indices_[i]++;
                plot_samples_[i].setData(
                    training_data_manager_.getSample(label, plot_sample_indices_[i]));
                assert(true == plot_samples_[i].clearContentModifiedFlag());
                populateSampleFeatures(i);
            }
        }
    }

    // Tab click detection
    const uint32_t left_margin = 10;
    const uint32_t top_margin = 20;
    const uint32_t tab_width = 120;
    if (x > left_margin && y < top_margin + 5) {
        if (x < left_margin + tab_width) {
            fragment_ = CALIBRATION;
        } else if (x < left_margin + 2 * tab_width) {
            fragment_ = PIPELINE;
        } else if (x < left_margin + 3 * tab_width) {
            fragment_ = ANALYSIS;
        } else if (x < left_margin + 4 * tab_width
                   && pipeline_->getClassifier() != nullptr) {
            fragment_ = TRAINING;
        }
    }
}

//--------------------------------------------------------------
void ofApp::mouseEntered(int x, int y) {

}

//--------------------------------------------------------------
void ofApp::mouseExited(int x, int y) {

}

//--------------------------------------------------------------
void ofApp::windowResized(int w, int h) {
}

//--------------------------------------------------------------
void ofApp::gotMessage(ofMessage msg) {

}

//--------------------------------------------------------------
void ofApp::dragEvent(ofDragInfo dragInfo) {

}