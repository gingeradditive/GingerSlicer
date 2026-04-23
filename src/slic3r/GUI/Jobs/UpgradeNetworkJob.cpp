#include "UpgradeNetworkJob.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/Utils/Http.hpp"

namespace Slic3r {
namespace GUI {

wxDEFINE_EVENT(EVT_UPGRADE_UPDATE_MESSAGE, wxCommandEvent);
wxDEFINE_EVENT(EVT_UPGRADE_NETWORK_SUCCESS, wxCommandEvent);
wxDEFINE_EVENT(EVT_DOWNLOAD_NETWORK_FAILED, wxCommandEvent);
wxDEFINE_EVENT(EVT_INSTALL_NETWORK_FAILED, wxCommandEvent);


UpgradeNetworkJob::UpgradeNetworkJob()
{
    name         = "plugins";
    package_name = "networking_plugins.zip";
}

void UpgradeNetworkJob::on_success(std::function<void()> success)
{
    m_success_fun = success;
}

void UpgradeNetworkJob::update_status(Ctl &ctl, int st, const std::string &msg)
{
    BOOST_LOG_TRIVIAL(info) << "UpgradeNetworkJob: percent = " << st << "msg = " << msg;
    ctl.update_status(st, msg);
    wxCommandEvent event(EVT_UPGRADE_UPDATE_MESSAGE);
    event.SetString(msg);
    event.SetEventObject(m_event_handle);
    wxPostEvent(m_event_handle, event);
}

void UpgradeNetworkJob::process(Ctl &ctl)
{
    // BambuLab network plugin download/install removed
    wxCommandEvent event(wxEVT_CLOSE_WINDOW);
    event.SetEventObject(m_event_handle);
    wxPostEvent(m_event_handle, event);
}

void UpgradeNetworkJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    try {
        if (eptr)
            std::rethrow_exception(eptr);
        eptr = nullptr;
    } catch (...) {
        eptr = std::current_exception();
    }

    if (canceled || eptr)
        return;
}

void UpgradeNetworkJob::set_event_handle(wxWindow *hanle)
{
    m_event_handle = hanle;
}

}} // namespace Slic3r::GUI
