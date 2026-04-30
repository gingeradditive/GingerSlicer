#include "Notebook.hpp"

//#ifdef _WIN32

#include "GUI_App.hpp"
#include "wxExtensions.hpp"
#include "Widgets/Button.hpp"

//BBS set font size
#include "Widgets/Label.hpp"

#include <wx/button.h>
#include <wx/sizer.h>

wxDEFINE_EVENT(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED, wxCommandEvent);

ButtonsListCtrl::ButtonsListCtrl(wxWindow *parent, wxBoxSizer* side_tools) :
    wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxTAB_TRAVERSAL)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__

    wxColour default_btn_bg = wxColour("#e7e7e7"); // Gray background

    SetBackgroundColour(default_btn_bg);

    int em = em_unit(this);// Slic3r::GUI::wxGetApp().em_unit();
    m_btn_margin = std::lround(0.3 * em);
    m_line_margin = std::lround(0.1 * em);

    m_sizer = new wxBoxSizer(wxHORIZONTAL);
    this->SetSizer(m_sizer);

    m_buttons_sizer = new wxFlexGridSizer(1, m_btn_margin, m_btn_margin);

    if (side_tools != NULL) {
        auto* left_tools_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto* right_tools_sizer = new wxBoxSizer(wxHORIZONTAL);

        for (size_t idx = 0; idx < side_tools->GetItemCount(); idx++) {
            wxSizerItem* item = side_tools->GetItem(idx);
            wxWindow* item_win = item->GetWindow();
            if (!item_win)
                continue;

            item_win->Reparent(this);

            if (idx < 2)
                left_tools_sizer->Add(item_win, item->GetProportion(), item->GetFlag(), item->GetBorder());
            else
                right_tools_sizer->Add(item_win, item->GetProportion(), item->GetFlag(), item->GetBorder());
        }

        auto* left_container = new wxBoxSizer(wxHORIZONTAL);
        left_container->Add(left_tools_sizer, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, m_btn_margin);
        left_container->AddStretchSpacer(1);

        auto* right_container = new wxBoxSizer(wxHORIZONTAL);
        right_container->AddStretchSpacer(1);
        right_container->Add(right_tools_sizer, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, m_btn_margin);

        // Balanced left/right sections keep tab buttons at absolute center.
        m_sizer->Add(left_container, 1, wxEXPAND);
        m_sizer->Add(m_buttons_sizer, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, m_btn_margin);
        m_sizer->Add(right_container, 1, wxEXPAND);
    } else {
        m_sizer->AddStretchSpacer(1);
        m_sizer->Add(m_buttons_sizer, 0, wxALIGN_CENTER | wxLEFT | wxTOP | wxBOTTOM, m_btn_margin);
        m_sizer->AddStretchSpacer(1);
    }

    // BBS: disable custom paint
    //this->Bind(wxEVT_PAINT, &ButtonsListCtrl::OnPaint, this);
    Bind(wxEVT_SYS_COLOUR_CHANGED, [this](auto& e){
    });
    Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& e) {
        // Rescale buttons when DPI changes (moving between monitors)
        Rescale();
        e.Skip();
    });
}

void ButtonsListCtrl::OnPaint(wxPaintEvent&)
{
    const wxSize sz = GetSize();
    wxPaintDC dc(this);

    if (m_selection < 0 || m_selection >= (int)m_pageButtons.size())
        return;

    wxColour selected_btn_bg("#1F8EEA");
    wxColour default_btn_bg("#463b3b"); // Gradient #414B4E
    const wxColour& btn_marker_color = Slic3r::GUI::wxGetApp().get_color_hovered_btn_label();

    // highlight selected notebook button

    for (int idx = 0; idx < int(m_pageButtons.size()); idx++) {
        Button* btn = m_pageButtons[idx];

        btn->SetBackgroundColor(idx == m_selection ? selected_btn_bg : default_btn_bg);

        wxPoint pos = btn->GetPosition();
        wxSize size = btn->GetSize();
        const wxColour& clr = idx == m_selection ? btn_marker_color : default_btn_bg;
        dc.SetPen(clr);
        dc.SetBrush(clr);
        dc.DrawRectangle(pos.x, pos.y + size.y, size.x, sz.y - size.y);
    }

#if 0
    // highlight selected mode button
    if (m_mode_sizer) {
        const std::vector<ModeButton*>& mode_btns = m_mode_sizer->get_btns();
        for (int idx = 0; idx < int(mode_btns.size()); idx++) {
            ModeButton* btn = mode_btns[idx];
            btn->SetBackgroundColor(btn->is_selected() ? selected_btn_bg : default_btn_bg);

            //wxPoint pos = btn->GetPosition();
            //wxSize size = btn->GetSize();
            //const wxColour& clr = btn->is_selected() ? btn_marker_color : default_btn_bg;
            //dc.SetPen(clr);
            //dc.SetBrush(clr);
            //dc.DrawRectangle(pos.x, pos.y + size.y, size.x, sz.y - size.y);
        }
    }
#endif

    // Draw orange bottom line

    dc.SetPen(btn_marker_color);
    dc.SetBrush(btn_marker_color);
    dc.DrawRectangle(1, sz.y - m_line_margin, sz.x, m_line_margin);
}

void ButtonsListCtrl::UpdateMode()
{
    //m_mode_sizer->SetMode(Slic3r::GUI::wxGetApp().get_mode());
}

void ButtonsListCtrl::Rescale()
{
    //m_mode_sizer->msw_rescale();
    int em = em_unit(this);
    for (Button* btn : m_pageButtons) {
        //BBS - Use same sizing as InsertPage to maintain consistency
        btn->SetMinSize({(int)((btn->GetLabel().empty() ? 36 : 136) * em / 10 * 1.5), (int)(36 * em / 10 * 1.5)});
        btn->Rescale();
    }

    m_btn_margin = std::lround(0.3 * em);
    m_line_margin = std::lround(0.1 * em);
    m_buttons_sizer->SetVGap(m_btn_margin);
    m_buttons_sizer->SetHGap(m_btn_margin);

    m_sizer->Layout();
}

void ButtonsListCtrl::SetSelection(int sel)
{
    if (m_selection == sel)
        return;
    // BBS: change button color
    wxColour selected_btn_bg("#d72828");    // Gradient #d72828
    if (m_selection >= 0) {
        StateColor bg_color = StateColor(
        std::pair{wxColour(240, 240, 240), (int) StateColor::Hovered},
        std::pair{wxColour(255, 255, 255), (int) StateColor::Normal});
        m_pageButtons[m_selection]->SetBackgroundColor(bg_color);
        StateColor text_color = StateColor(
        std::pair{wxColour(0, 0, 0), (int) StateColor::Normal}
        );
        m_pageButtons[m_selection]->SetSelected(false);
        m_pageButtons[m_selection]->SetTextColor(text_color);
        m_pageButtons[m_selection]->SetBorderColor(wxColour(240, 240, 240));
        m_pageButtons[m_selection]->SetBorderWidth(1);
        m_pageButtons[m_selection]->SetCornerRadius(16);
    }
    m_selection = sel;

    StateColor bg_color = StateColor(
        std::pair{wxColour(255, 255, 255), (int) StateColor::Hovered},
        std::pair{wxColour(255, 255, 255), (int) StateColor::Normal});
    m_pageButtons[m_selection]->SetBackgroundColor(bg_color);

    StateColor text_color = StateColor(
        std::pair{wxColour(0, 0, 0), (int) StateColor::Normal}
        );
    m_pageButtons[m_selection]->SetSelected(true);
    m_pageButtons[m_selection]->SetTextColor(text_color);
    m_pageButtons[m_selection]->SetBorderColor(wxColour("#d72828"));
    m_pageButtons[m_selection]->SetBorderWidth(4);
    m_pageButtons[m_selection]->SetCornerRadius(16);
    
    Refresh();
}

bool ButtonsListCtrl::InsertPage(size_t n, const wxString &text, bool bSelect /* = false*/, const std::string &bmp_name /* = ""*/, const std::string &inactive_bmp_name)
{
    Button * btn = new Button(this, text.empty() ? text : " " + text, bmp_name, wxNO_BORDER);
    btn->SetCornerRadius(0);

    int em = em_unit(this);
    //BBS set size for button
    btn->SetMinSize({(int)((text.empty() ? 36 : 136) * em / 10 * 1.5), (int)(36 * em / 10 * 1.5)});

    StateColor bg_color = StateColor(
        std::pair{wxColour(240, 240, 240), (int) StateColor::Hovered},
        std::pair{wxColour(255, 255, 255), (int) StateColor::Normal});

    btn->SetBackgroundColor(bg_color);
    StateColor text_color = StateColor(
        std::pair{wxColour(0, 0, 0), (int) StateColor::Normal});
    btn->SetTextColor(text_color);
    btn->SetInactiveIcon(inactive_bmp_name);
    btn->SetSelected(false);
    btn->SetBorderColor(wxColour(240, 240, 240));
    btn->SetBorderWidth(1);
    btn->SetCornerRadius(16);
    btn->Bind(wxEVT_BUTTON, [this, btn](wxCommandEvent& event) {
        if (auto it = std::find(m_pageButtons.begin(), m_pageButtons.end(), btn); it != m_pageButtons.end()) {
            auto sel = it - m_pageButtons.begin();
            //do it later
            //SetSelection(sel);
            
            wxCommandEvent evt = wxCommandEvent(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED);
            evt.SetId(sel);
            wxPostEvent(this->GetParent(), evt);
        }
    });
    m_pageButtons.insert(m_pageButtons.begin() + n, btn);
    m_buttons_sizer->Insert(n, new wxSizerItem(btn));
    m_buttons_sizer->SetCols(m_buttons_sizer->GetCols() + 1);
    m_sizer->Layout();
    return true;
}

void ButtonsListCtrl::RemovePage(size_t n)
{
    Button* btn = m_pageButtons[n];
    m_pageButtons.erase(m_pageButtons.begin() + n);
    m_buttons_sizer->Remove(n);
#if __WXOSX__
    RemoveChild(btn);
#else
    btn->Reparent(nullptr);
#endif
    btn->Destroy();
    m_sizer->Layout();
}

bool ButtonsListCtrl::SetPageImage(size_t n, const std::string& bmp_name) const
{
    if (n >= m_pageButtons.size())
        return false;
     
    // BBS
    //return m_pageButtons[n]->SetBitmap_(bmp_name);
    ScalableBitmap bitmap(NULL, bmp_name);
    //m_pageButtons[n]->SetBitmap_(bitmap);
    return true;
}

void ButtonsListCtrl::SetPageText(size_t n, const wxString& strText)
{
    Button* btn = m_pageButtons[n];
    btn->SetLabel(strText);
}

wxString ButtonsListCtrl::GetPageText(size_t n) const
{
    Button* btn = m_pageButtons[n];
    return btn->GetLabel();
}

//#endif // _WIN32

void Notebook::Init()
{
    // We don't need any border as we don't have anything to separate the
    // page contents from.
    SetInternalBorder(0);

    // No effects by default.
    m_showEffect = m_hideEffect = wxSHOW_EFFECT_NONE;

    m_showTimeout = m_hideTimeout = 0;

    /* On Linux, Gstreamer wxMediaCtrl does not seem to get along well with
     * 32-bit X11 visuals (the overlay does not work).  Is this a wxWindows
     * bug?  Is this a Gstreamer bug?  No idea, but it is our problem ... 
     * and anyway, this transparency thing just isn't all that interesting,
     * so we just don't do it on Linux. 
     */
#ifndef __WXGTK__
    SetBackgroundStyle(wxBG_STYLE_TRANSPARENT);
#endif
}
