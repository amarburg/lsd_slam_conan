/**
*
* Based on original LSD-SLAM code from:
* Copyright 2013 Jakob Engel <engelj at in dot tum dot de> (Technical University of Munich)
* For more information see <http://vision.in.tum.de/lsdslam>
*
* LSD-SLAM is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* LSD-SLAM is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with LSD-SLAM. If not, see <http://www.gnu.org/licenses/>.
*/

#include <opencv2/opencv.hpp>

#include <boost/thread.hpp>

#include <tclap/CmdLine.h>

#include <g3log/g3log.hpp>
#include <g3log/logworker.hpp>
#include "util/G3LogSinks.h"

#include "util/settings.h"
#include "util/Parse.h"
#include "util/globalFuncs.h"
#include "util/ThreadMutexObject.h"
#include "util/Configuration.h"

#include "LSD/LSD.h"

#ifdef USE_ZED
#include "util/ZedUtils.h"
#endif

using namespace lsd_slam;

ThreadMutexObject<bool> lsdDone(false), guiDone(false);

ThreadSynchronizer lsdReady, guiReady, startAll;

int main( int argc, char** argv )
{
  auto worker = g3::LogWorker::createLogWorker();
  auto handle = worker->addDefaultLogger(argv[0], ".");
  auto stderrHandle = worker->addSink(std::unique_ptr<ColorStderrSink>( new ColorStderrSink ),
                                       &ColorStderrSink::ReceiveLogMessage);

  g3::initializeLogging(worker.get());
  std::future<std::string> log_file_name = handle->call(&g3::FileSink::fileName);
  std::cout << "*\n*   Log file: [" << log_file_name.get() << "]\n\n" << std::endl;

  LOG(INFO) << "Starting log.";

  DataSource *dataSource = NULL;
  Undistorter* undistorter = NULL;

  Configuration conf;

  bool doGui = true;

    try {
      TCLAP::CmdLine cmd("LSD", ' ', "0.1");

      TCLAP::ValueArg<std::string> calibFileArg("c", "calib", "Calibration file", false, "", "Calibration filename", cmd );
      TCLAP::ValueArg<std::string> resolutionArg("r", "resolution", "", false, "hd1080", "{hd2k, hd1080, hd720, vga}", cmd );

#ifdef USE_ZED
      TCLAP::SwitchArg zedSwitch("","zed","Use ZED", cmd, false);
      TCLAP::ValueArg<std::string> svoFileArg("","svo-input","Name of SVO file to read",false,"","SVO filename", cmd);
      TCLAP::SwitchArg depthSwitch("","depth","Use depth data", cmd, false);
#endif

      TCLAP::ValueArg<std::string> logFileArg("","log-input","Name of logger file to read",false,"","Logger filename", cmd);

      TCLAP::SwitchArg debugOutputSwitch("","debug-to-console","Print DEBUG output to console", cmd, false);
      TCLAP::SwitchArg noGuiSwitch("","no-gui","Do not run GUI", cmd, false);
      TCLAP::ValueArg<int> fpsArg("", "fps","FPS", false, 0, "", cmd );

      TCLAP::UnlabeledMultiArg<std::string> imageFilesArg("input-files","Name of image files / directories to read", false, "Files or directories", cmd );

      cmd.parse(argc, argv );

      if( debugOutputSwitch.getValue() ) stderrHandle->call( &ColorStderrSink::setThreshold, DEBUG );

#ifdef USE_ZED
      if( zedSwitch.getValue() || svoFileArg.isSet() ) {
        if( depthSwitch.getValue() ) {
          LOG(INFO) << "Using depth data from Stereolabs libraries";
          conf.doDepth = Configuration::STEREO_ZED;
        }

        const sl::zed::MODE zedMode = ( conf.doDepth == Configuration::STEREO_ZED ) ? sl::zed::MODE::QUALITY : sl::zed::MODE::NONE;
        const int whichGpu = -1;
        const bool verboseInit = true;

        sl::zed::Camera *camera = NULL;

        if( svoFileArg.isSet() )
      	{
          LOG(INFO) << "Loading SVO file " << svoFileArg.getValue();
          camera = new sl::zed::Camera( svoFileArg.getValue() );
      	} else {
          const sl::zed::ZEDResolution_mode zedResolution = parseResolution( resolutionArg.getValue() );
          LOG(INFO) << "Using live Zed data";
          camera = new sl::zed::Camera( zedResolution, fpsArg.getValue() );
          conf.stopOnFailedRead = false;
        }

				#ifdef ZED_1_0
								sl::zed::InitParams initParams;
								initParams.mode = zedMode;
								initParams.verbose = verboseInit;
				        sl::zed::ERRCODE err = camera->init( initParams );
				#else
								sl::zed::ERRCODE err = camera->init( zedMode, whichGpu, verboseInit );
				#endif

        if (err != sl::zed::SUCCESS) {
          LOG(WARNING) << "Unable to init the zed: " << errcode2str(err);
          delete camera;
          exit(-1);
        }

        dataSource = new ZedSource( camera, conf.doDepth == Configuration::STEREO_ZED );
        if( fpsArg.isSet() && svoFileArg.isSet() ) dataSource->setFPS( fpsArg.getValue() );
        undistorter = new UndistorterZED( camera );
      } else
#endif
      {
        std::vector< std::string > imageFiles = imageFilesArg.getValue();

        if( logFileArg.isSet() ) {
          dataSource = new LoggerSource( logFileArg.getValue() );
        } else if ( imageFiles.size() > 0 && fs::path(imageFiles[0]).extension().string() == ".log" ) {
          dataSource = new LoggerSource( imageFiles[0] );
        } else {
          dataSource = new ImagesSource( imageFiles );
        }

        if( fpsArg.isSet() ) dataSource->setFPS( fpsArg.getValue() );

        if( !calibFileArg.isSet() ) {
          LOG(WARNING) << "Must specify camera calibration!";
          exit(-1);
        }

        undistorter = Undistorter::getUndistorterForFile(calibFileArg.getValue());
        CHECK(undistorter != NULL);
      }

      doGui = !noGuiSwitch.getValue();

    } catch (TCLAP::ArgException &e)  // catch any exceptions
  	{
      LOG(WARNING) << "error: " << e.error() << " for arg " << e.argId();
      exit(-1);
    }

#ifdef ENABLE_SSE
  LOG(INFO) << "With SSE optimizations.";
#elif ENABLE_NEON
  LOG(INFO) << "With NEON optimizations.";
#endif

  CHECK( undistorter != NULL ) << "Could not create undistorter.";
  CHECK( dataSource != NULL ) << "Could not create data source.";

  // Load the configuration object

  conf.inputImage = undistorter->inputImageSize();
  conf.slamImage  = undistorter->outputImageSize();
  conf.camera     = undistorter->getCamera();

  LOG(INFO) << "Slam image: " << conf.slamImage.width << " x " << conf.slamImage.height;

  CHECK( (conf.camera.fx) > 0 && (conf.camera.fy > 0) ) << "Camera focal length is zero";

	SlamSystem * system = new SlamSystem(conf);

  if( doGui ) {
    LOG(INFO) << "Starting GUI thread";
    boost::thread guiThread(runGui, system );
    guiReady.wait();
  }

  LOG(INFO) << "Starting input thread.";
  boost::thread inputThread(runInput, system, dataSource, undistorter );
  lsdReady.wait();

  // Wait for all threads to be ready.
  LOG(INFO) << "Starting all threads.";
  startAll.notify();

  while(true)
  {
      if( (lsdDone.getValue() || guiDone.getValue()) && !system->finalized)
      {
          LOG(INFO) << "Finalizing system.";
          system->finalize();
      }

    sleep(1);
  }


  if( system ) delete system;
  if( undistorter ) delete undistorter;

  return 0;
}
