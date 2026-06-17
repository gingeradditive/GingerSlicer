#ifndef CATCH_MAIN
#define CATCH_MAIN

#define CATCH_CONFIG_EXTERNAL_INTERFACES
#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_DEFAULT_REPORTER "verboseconsole"
#include <catch2/catch.hpp>

#include <boost/filesystem.hpp>
#include "libslic3r/Utils.hpp"

namespace Catch {

// The GUI app sets a data dir (and temporary dir) at startup; the test binaries did not, which broke
// every test that slices/exports through the real pipeline (3mf backup/restore on load, g-code export)
// with "create_directory ... cannot find the path". Set them once, before any test runs, so the full
// slicing pipeline is exercisable headless instead of skipped.
struct GingerTestEnvListener : Catch::TestEventListenerBase {
    using TestEventListenerBase::TestEventListenerBase;
    void testRunStarting(Catch::TestRunInfo const &) override {
        boost::filesystem::path base = boost::filesystem::temp_directory_path() / "gingerslicer_tests";
        boost::filesystem::create_directories(base);
        boost::filesystem::create_directories(base / "tmp");
        Slic3r::set_data_dir(base.string());
        Slic3r::set_temporary_dir((base / "tmp").string());
        // Mirror the headless CLI startup (GingerSlicer.cpp): the slicing/export pipeline reads
        // resources (thumbnails, templates, i18n). TEST_DATA_DIR is "<repo>/tests/data".
#ifdef TEST_DATA_DIR
        boost::filesystem::path repo = boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path();
        boost::filesystem::path res  = repo / "resources";
        if (boost::filesystem::exists(res)) {
            Slic3r::set_resources_dir(res.string());
            Slic3r::set_var_dir((res / "images").string());
            Slic3r::set_local_dir((res / "i18n").string());
        }
#endif
    }
};
CATCH_REGISTER_LISTENER(GingerTestEnvListener)

struct VerboseConsoleReporter : public ConsoleReporter {
    double duration = 0.;
    using ConsoleReporter::ConsoleReporter;
    
    void testCaseStarting(TestCaseInfo const& _testInfo) override
    {
        Colour::use(Colour::Cyan);
        stream << "Testing ";
        Colour::use(Colour::None);
        stream << _testInfo.name << std::endl;
        ConsoleReporter::testCaseStarting(_testInfo);
    }
    
    void sectionStarting(const SectionInfo &_sectionInfo) override
    {
        if (_sectionInfo.name != currentTestCaseInfo->name)
            stream << _sectionInfo.name << std::endl;
        
        ConsoleReporter::sectionStarting(_sectionInfo);
    }
    
    void sectionEnded(const SectionStats &_sectionStats) override {
        duration += _sectionStats.durationInSeconds;
        ConsoleReporter::sectionEnded(_sectionStats);
    } 
    
    void testCaseEnded(TestCaseStats const& stats) override
    {
        if (stats.totals.assertions.allOk()) {
            Colour::use(Colour::BrightGreen);
            stream << "Passed";
            Colour::use(Colour::None);
            stream << " in " << duration << " [seconds]\n" << std::endl;
        }
        
        duration = 0.;            
        ConsoleReporter::testCaseEnded(stats);
    }
};

CATCH_REGISTER_REPORTER( "verboseconsole", VerboseConsoleReporter )

} // namespace Catch

#endif // CATCH_MAIN
