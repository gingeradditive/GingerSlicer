#ifndef slic3r_calib_dlg_hpp_
#define slic3r_calib_dlg_hpp_

#include "wxExtensions.hpp"
#include "GUI_Utils.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/RoundedRectangle.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/LabeledStaticBox.hpp"
#include "Widgets/RadioGroup.hpp"
#include "GUI_App.hpp"
#include "wx/hyperlink.h"
#include <wx/radiobox.h>
#include "libslic3r/calib.hpp"

namespace Slic3r { namespace GUI {

class Param_Sweep_Dlg : public DPIDialog
{
public:
    Param_Sweep_Dlg(wxWindow* parent, wxWindowID id, Plater* plater);
    ~Param_Sweep_Dlg();
    void on_dpi_changed(const wxRect& suggested_rect) override;

protected:
    virtual void on_start(wxCommandEvent& event);
    virtual void on_param_changed(wxCommandEvent& event);
    Calib_Params m_params;

    ComboBox* m_cbParam;
    TextInput* m_tiStart;
    TextInput* m_tiEnd;
    TextInput* m_tiStep;
    Plater* m_plater;
};
}} // namespace Slic3r::GUI
#endif
