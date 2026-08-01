#pragma once
#include "NeoModulePanel.hpp"
namespace neogui::ui {
class GUIEditorPanel : public neomodules::Panel {
public:
    using Panel::Panel;
    virtual bool openFile(const std::filesystem::path&)=0;
    virtual bool openResource(neoshared::ResourceDocument)=0;
};
GUIEditorPanel* createEditorPanel(wxWindow* parent,neomodules::Context context={});
}
