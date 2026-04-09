#ifndef slic3r_ConfigWizard_hpp_
#define slic3r_ConfigWizard_hpp_

namespace Slic3r {
namespace GUI {

// Setup Wizard has been removed. This struct is kept only
// for the RunReason / StartPage enums referenced elsewhere.
struct ConfigWizard
{
    enum RunReason {
        RR_DATA_EMPTY,
        RR_DATA_LEGACY,
        RR_DATA_INCOMPAT,
        RR_USER,
    };

    enum StartPage {
        SP_WELCOME,
        SP_PRINTERS,
        SP_FILAMENTS,
        SP_MATERIALS,
        SP_CUSTOM,
    };
};

}
}

#endif
