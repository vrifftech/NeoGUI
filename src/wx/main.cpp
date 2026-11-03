#include "wx/GUIEditorPanel.hpp"
#include "NeoSettings.hpp"
#include <wx/app.h>
namespace {
class Frame final : public wxFrame {
public:
    Frame():wxFrame(nullptr,wxID_ANY,"NeoGUI") {
        neomodules::Context context;context.titleChanged=[this](const wxString& title){SetTitle(title);};context.closeRequested=[this]{Close();};
        panel_=neogui::ui::createEditorPanel(this,std::move(context));SetMenuBar(panel_->takeMenus().release());
        auto* box=new wxBoxSizer(wxVERTICAL);box->Add(panel_,1,wxEXPAND);SetSizer(box);
        Bind(wxEVT_MENU,[this](wxCommandEvent& event){if(!neomodules::routeCommand({panel_},event))event.Skip();});
        Bind(wxEVT_MENU_OPEN,[this](wxMenuEvent& event){neomodules::routeMenuOpen({panel_},event);event.Skip();});
        Bind(wxEVT_CLOSE_WINDOW,[this](wxCloseEvent& event){if(!panel_->canClose())event.Veto();else {settings_.saveWindowPlacement(*this);event.Skip();}});
        wxui::configureResponsiveWindow(*this,wxSize(1320,850),wxSize(900,600));settings_.restoreWindowPlacement(*this);
    }
    ~Frame() override {DestroyChildren();}
    neogui::ui::GUIEditorPanel* panel_{};
private:neosettings::AppSettings settings_{"NeoGUI"};
};
class App final : public wxApp {
public:bool OnInit() override {SetAppName("NeoGUI");SetVendorName("NeoTools");wxInitAllImageHandlers();auto* frame=new Frame;frame->Show();if(argc>1){const auto path=neosettings::pathFromWx(wxString(argv[1]));frame->CallAfter([frame,path]{frame->panel_->openFile(path);});}return true;}
};
}
wxIMPLEMENT_APP(App);
