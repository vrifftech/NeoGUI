#include "wx/GUIEditorPanel.hpp"
#include "GuiRenderer.hpp"
#include "NeoSettings.hpp"
#include "NeoViewState.hpp"
#include "NeoGameDirectoryMenu.hpp"
#include <wx/textctrl.h>
#include <cctype>
#include <gff/AppModel.hpp>
#include <neoshared/GffResourceDocument.hpp>
#include <neoshared/material/Material.hpp>
#include <neoshared/texture/FileUtil.hpp>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dcbuffer.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>
#include <wx/treectrl.h>
#include <wx/wrapsizer.h>
#include <algorithm>
#include <cmath>
#include <map>

namespace neogui::ui {
namespace {
enum {ID_Open=wxID_HIGHEST+25000,ID_Save,ID_SaveAs,ID_Undo,ID_Redo,ID_Assets,ID_HostAssets,ID_Tlk,
      ID_DarkMode,ID_FontIncrease,ID_FontDecrease,ID_FontReset,ID_Exit,ID_About,ID_ClearRecent,
      ID_RecentFirst,ID_RecentLast=ID_RecentFirst+neosettings::kMaxRecentFiles-1};
using Resolver=std::function<std::optional<neoshared::ResourceDocument>(const std::string&,std::uint16_t)>;
using Key=std::pair<std::string,std::uint16_t>;
constexpr std::pair<std::uint16_t,const char*> textureTypes[]={{3007,".tpc"},{3006,".txb"},{3,".tga"},{2033,".dds"}};

class Canvas final : public wxPanel {
public:
    Canvas(wxWindow* parent,GuiRenderer& renderer,const std::function<const neotlk::TlkLookup&()>& tlk,
           std::function<void(int)> select,std::function<void(Rect)> edit)
        :wxPanel(parent),renderer_(renderer),tlk_(tlk),select_(std::move(select)),edit_(std::move(edit)) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);SetMinSize(FromDIP(wxSize(300,250)));
        SetToolTip("Click a control to select it. Drag to move; drag its lower-right handle to resize. Esc cancels a drag.");
        Bind(wxEVT_PAINT,&Canvas::paint,this);
        Bind(wxEVT_SIZE,[this](wxSizeEvent& event){Refresh();event.Skip();});
        Bind(wxEVT_LEFT_DOWN,&Canvas::down,this);Bind(wxEVT_MOTION,&Canvas::motion,this);
        Bind(wxEVT_LEFT_UP,[this](wxMouseEvent&){
            if(!gesture_)return;
            const auto result=*gesture_;gesture_.reset();ReleaseMouse();Refresh();
            if(result!=original_)edit_(result);
        });
        Bind(wxEVT_MOUSE_CAPTURE_LOST,[this](wxMouseCaptureLostEvent&){gesture_.reset();Refresh();});
        Bind(wxEVT_CHAR_HOOK,[this](wxKeyEvent& event){
            if(event.GetKeyCode()==WXK_ESCAPE&&gesture_) {gesture_.reset();ReleaseMouse();Refresh();}
            else event.Skip();
        });
    }
    void setDocument(const GffStruct* root,int selected) {if(HasCapture())ReleaseMouse();root_=root;selected_=selected;gesture_.reset();Refresh();}
    void select(int selected) {selected_=selected;Refresh();}
    void setAssets(bool value) {assets_=value;Refresh();}
    void setZoom(double value) {zoom_=value;Refresh();}
    void setGrid(int value) {grid_=value;}
    void setState(int value) {state_=value;Refresh();}
private:
    Rect rect(int index) const {return index==selected_&&gesture_?*gesture_:extent(*controls(*root_).GetStruct(std::size_t(index)));}
    wxPoint2DDouble point(wxPoint value) const {return {(value.x-offsetX_)/scale_,(value.y-offsetY_)/scale_};}
    void down(wxMouseEvent& event) {
        if(!root_)return;
        SetFocus();const auto p=point(event.GetPosition());int hit=-1;
        if(selected_>=0) {const auto r=rect(selected_);resizing_=std::abs(p.m_x-r.GetRight())<=7/scale_&&std::abs(p.m_y-r.GetBottom())<=7/scale_;if(resizing_)hit=selected_;}
        if(hit<0) {resizing_=false;for(std::size_t i=controls(*root_).count();i>0;--i)if(rect(int(i-1)).Contains(p)) {hit=int(i-1);break;}}
        selected_=hit;select_(hit);Refresh();
        if(hit>=0) {original_=rect(hit);gesture_=original_;start_=p;CaptureMouse();}
    }
    void motion(wxMouseEvent& event) {
        if(!gesture_)return;
        const auto p=point(event.GetPosition());const auto snap=[&](double value){return std::round(value/grid_)*grid_;};
        auto r=original_;
        if(resizing_) {r.m_width=std::max(1.0,snap(original_.m_width+p.m_x-start_.m_x));r.m_height=std::max(1.0,snap(original_.m_height+p.m_y-start_.m_y));}
        else {r.m_x=snap(original_.m_x+p.m_x-start_.m_x);r.m_y=snap(original_.m_y+p.m_y-start_.m_y);}
        gesture_=r;Refresh();
    }
    void drawControl(wxGraphicsContext& dc,const GffStruct& control,Rect r) {
        const auto type=std::stoul(string(control,"CONTROLTYPE"));
        if(type==9) {
            renderer_.scrollbar(dc,control,r,integer(control,"MAXVALUE"),integer(control,"CURVALUE"),integer(control,"VISIBLEVALUE"));return;
        }
        const bool highlight=state_==1||state_==3,toggled=type==7&&(state_>=2||byte(control,"ISSELECTED"));
        if(type==8) {renderer_.slider(dc,control,r,highlight);return;}
        const char* border=toggled?(highlight?"HILIGHTSELECTED":"SELECTED"):highlight&&(type==5||type==6||type==7)?"HILIGHT":"BORDER";
        const auto* bar=type==11?field<GffStruct>(control,"SCROLLBAR"):nullptr;
        auto content=r;
        if(bar) {const auto width=renderer_.layoutExtent(extent(*bar)).m_width;content.m_width-=width;if(byte(control,"LEFTSCROLLBAR",1))content.m_x+=width;}
        auto inner=renderer_.border(dc,control,border,content);
        if(type==5||type==6) {inner=renderer_.borderInner(control,"BORDER",content);inner.Intersect(renderer_.borderInner(control,"HILIGHT",content));}
        else if(type==7)inner=content;
        if(type==10) {
            auto progress=inner;const auto maximum=std::max(0,integer(control,"MAXVALUE"));
            const auto value=std::clamp(integer(control,"CURVALUE"),0,maximum);
            if(maximum>0) {
                const bool vertical=r.m_height>r.m_width;
                const auto length=vertical?progress.m_height:progress.m_width;
                const auto filled=int(float(value)/float(maximum)*float(length));
                if(!byte(control,"STARTFROMLEFT",1)) {if(vertical)progress.m_y+=length-filled;else progress.m_x+=length-filled;}
                if(vertical)progress.m_height=filled;else progress.m_width=filled;
            }
            renderer_.border(dc,control,"PROGRESS",progress);
        }
        if(const auto* text=field<GffStruct>(control,"TEXT")) {
            auto value=neogff::decodeGffText(string(*text,"TEXT"),tlk_().languageId());
            if(const auto* ref=field<GffUInt32Field>(*text,"STRREF");ref&&ref->value!=0xffffffffu)value=tlk_().resolveKotor(ref->value).value();
            renderer_.text(dc,*text,value,inner,renderer_.fontScale());
        }
        if(bar) {
            const auto width=renderer_.layoutExtent(extent(*bar)).m_width;
            renderer_.scrollbar(dc,*bar,{byte(control,"LEFTSCROLLBAR",1)?r.m_x:r.GetRight()-width,r.m_y,width,r.m_height},1,1,1);
        }
    }
    void paint(wxPaintEvent&) {
        wxAutoBufferedPaintDC paint(this);paint.SetBackground(wxBrush(GetBackgroundColour()));paint.Clear();
        if(!root_) {paint.SetTextForeground(GetForegroundColour());paint.DrawText("Open a .gui layout from the game browser or a working file.",FromDIP(14),FromDIP(14));return;}
        try {
            const auto root=extent(*root_);renderer_.setViewport(renderer_.layoutExtent({0,0,root.m_width,root.m_height}));const auto size=GetClientSize();
            scale_=zoom_?zoom_:std::min((size.x-24)/root.m_width,(size.y-24)/root.m_height);
            offsetX_=(size.x-root.m_width*scale_)/2;offsetY_=(size.y-root.m_height*scale_)/2;
            std::unique_ptr<wxGraphicsContext> dc(wxGraphicsContext::Create(paint));dc->Translate(offsetX_,offsetY_);dc->Scale(scale_,scale_);
            dc->SetPen(wxPen(wxColour(100,100,100),1/scale_));dc->SetBrush(wxBrush(wxColour(12,14,18)));dc->DrawRectangle(0,0,root.m_width,root.m_height);
            if(assets_) {
                dc->PushState();dc->Scale(1/renderer_.layoutScale(),1/renderer_.layoutScale());
                const auto nativeRoot=renderer_.layoutExtent({0,0,root.m_width,root.m_height});
                dc->BeginLayer(std::clamp(number(*root_,"ALPHA",1),0.0,1.0));
                if(field<GffStruct>(*root_,"BORDER"))renderer_.border(*dc,*root_,"BORDER",nativeRoot);
                else renderer_.stamp(*dc,string(*root_,"BACKGROUND"),nativeRoot,{1,1,1});
                for(std::size_t i=0;i<controls(*root_).count();++i)drawControl(*dc,*controls(*root_).GetStruct(i),renderer_.layoutExtent(rect(int(i))));
                dc->EndLayer();dc->PopState();
            }
            dc->SetBrush(*wxTRANSPARENT_BRUSH);
            for(std::size_t i=0;i<controls(*root_).count();++i) {
                const auto r=rect(int(i));dc->SetPen(wxPen(int(i)==selected_?wxColour(255,205,70):wxColour(100,150,185),1/scale_));
                dc->DrawRectangle(r.m_x,r.m_y,r.m_width,r.m_height);
            }
            if(selected_>=0) {const auto r=rect(selected_);dc->SetBrush(wxBrush(wxColour(255,205,70)));dc->DrawRectangle(r.GetRight()-4/scale_,r.GetBottom()-4/scale_,8/scale_,8/scale_);}
        } catch(const std::exception& error) {paint.SetTextForeground(wxColour(255,160,140));paint.DrawText(wxui::toWx(error.what()),12,12);}
    }
    GuiRenderer& renderer_;
    std::function<const neotlk::TlkLookup&()> tlk_;
    std::function<void(int)> select_;
    std::function<void(Rect)> edit_;
    const GffStruct* root_{};
    int selected_{-1},grid_{1},state_{};
    bool assets_{},resizing_{};
    double scale_{1},zoom_{},offsetX_{},offsetY_{};
    wxPoint2DDouble start_;
    Rect original_;
    std::optional<Rect> gesture_;
};
class ControlData final : public wxTreeItemData {
public:explicit ControlData(int value):index(value) {} int index;
};
class Editor final : public GUIEditorPanel {
public:
    Editor(wxWindow* parent,neomodules::Context context):GUIEditorPanel(parent,std::move(context)),renderer_([this](const auto& name)->const auto&{return textures_.at(neoshared::material::textureResourceKey(name));}) {
        darkMode_=settings_.darkMode();fontScale_=settings_.fontScale();
        auto* box=new wxBoxSizer(wxVERTICAL);auto* toolbar=new wxWrapSizer(wxHORIZONTAL);
        const auto button=[&](int id,const char* title){auto* control=new wxButton(this,id,title);toolbar->Add(control,0,wxALL,4);};
        button(ID_Open,"Open...");button(ID_Save,"Save");button(ID_SaveAs,"Save As...");button(ID_Undo,"Undo");button(ID_Redo,"Redo");
        game_=new wxChoice(this,wxID_ANY);game_->Append("K1");game_->Append("K2");game_->SetSelection(0);toolbar->Add(game_,0,wxALL,4);
        layoutSource_=new wxChoice(this,wxID_ANY);layoutSource_->Append("Archive layout");layoutSource_->Append("Loose / Override layout");layoutSource_->SetSelection(0);toolbar->Add(layoutSource_,0,wxALL,4);
        preview_=new wxCheckBox(this,wxID_ANY,"Game assets");toolbar->Add(preview_,0,wxALIGN_CENTER_VERTICAL|wxALL,4);
        auto* zoom=new wxChoice(this,wxID_ANY);for(const auto* item:{"Fit","50%","100%","200%"})zoom->Append(item);zoom->SetSelection(0);toolbar->Add(zoom,0,wxALL,4);
        auto* grid=new wxChoice(this,wxID_ANY);for(const auto* item:{"1 px grid","4 px grid","8 px grid","16 px grid"})grid->Append(item);grid->SetSelection(0);toolbar->Add(grid,0,wxALL,4);
        auto* state=new wxChoice(this,wxID_ANY);for(const auto* item:{"Normal","Highlight","Selected","Selected + highlight"})state->Append(item);state->SetSelection(0);toolbar->Add(state,0,wxALL,4);
        box->Add(toolbar,0,wxEXPAND);
        auto* split=new wxSplitterWindow(this,wxID_ANY);tree_=new wxTreeCtrl(split,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxTR_HAS_BUTTONS|wxTR_SINGLE);
        auto* right=new wxSplitterWindow(split,wxID_ANY);canvas_=new Canvas(right,renderer_,[this]()->const auto&{return model_->tlk();},[this](int index){tree_->SelectItem(items_.at(std::size_t(index+1)));},[this](Rect r){editExtent(r);});
        auto* inspector=new wxPanel(right);auto* props=new wxBoxSizer(wxVERTICAL);auto* dimensions=new wxFlexGridSizer(0,4,4,4);
        for(std::size_t i=0;i<geometry_.size();++i) {dimensions->Add(new wxStaticText(inspector,wxID_ANY,std::array<const char*,4>{"Left","Top","Width","Height"}[i]),0,wxALIGN_CENTER_VERTICAL);geometry_[i]=new wxTextCtrl(inspector,wxID_ANY,{},wxDefaultPosition,FromDIP(wxSize(90,-1)),wxTE_PROCESS_ENTER);dimensions->Add(geometry_[i],1,wxEXPAND);geometry_[i]->Bind(wxEVT_TEXT_ENTER,[this](wxCommandEvent&){applyExtent();});}
        dimensions->AddGrowableCol(1);dimensions->AddGrowableCol(3);props->Add(dimensions,0,wxEXPAND|wxALL,6);
        apply_=new wxButton(inspector,wxID_ANY,"Apply geometry");props->Add(apply_,0,wxLEFT|wxRIGHT|wxBOTTOM,6);apply_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){applyExtent();});
        all_=new wxCheckBox(inspector,wxID_ANY,"Show all layout fields");props->Add(all_,0,wxALL,6);all_->Bind(wxEVT_CHECKBOX,[this](wxCommandEvent&){showProperties();});
        properties_=new wxListCtrl(inspector,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxLC_REPORT|wxLC_SINGLE_SEL);
        properties_->InsertColumn(0,"Field",wxLIST_FORMAT_LEFT,FromDIP(200));properties_->InsertColumn(1,"Type",wxLIST_FORMAT_LEFT,FromDIP(110));properties_->InsertColumn(2,"Value",wxLIST_FORMAT_LEFT,FromDIP(300));props->Add(properties_,1,wxEXPAND|wxALL,6);
        auto* note=new wxStaticText(inspector,wxID_ANY,"Double-click a field to edit its native value. Game assets needs matching textures, fonts and a talk table (File menu). K1 preview uses a 1024x768 desktop with the selected layout source. Game scripts are not executed.");note->Wrap(FromDIP(320));props->Add(note,0,wxEXPAND|wxALL,6);inspector->SetSizer(props);
        right->SetMinimumPaneSize(FromDIP(260));right->SetSashGravity(0.7);right->SplitVertically(canvas_,inspector,-FromDIP(360));
        split->SetMinimumPaneSize(FromDIP(150));split->SplitVertically(tree_,right,FromDIP(210));box->Add(split,1,wxEXPAND);SetSizer(box);createModuleStatusBar(1);
        auto* bar=new wxMenuBar;auto* file=new wxMenu;
        file->Append(ID_Open,"&Open GUI...\tCtrl+O");
        recentFilesMenu_=new wxMenu;refreshRecentFiles();file->AppendSubMenu(recentFilesMenu_,"Open &Recent");
        file->Append(ID_Save,"&Save\tCtrl+S");file->Append(ID_SaveAs,"Save &As...\tCtrl+Shift+S");
        file->AppendSeparator();file->Append(ID_Assets,"Asset folder...");file->Append(ID_HostAssets,"Use game-browser assets");file->Append(ID_Tlk,"Load talk table...");
        gameDirectoryMenu_=neogames::appendOpenGameDirectoryMenu(*this,*file,
            [this](const std::filesystem::path& directory){chooseOpen(directory);},neogames::GameDirectoryGameIds{"kotor","kotor2"});
        if(!context_.embedded) {file->AppendSeparator();file->Append(ID_Exit,"E&xit");}
        bar->Append(file,"&File");
        auto* edit=new wxMenu;edit->Append(ID_Undo,"&Undo\tCtrl+Z");edit->Append(ID_Redo,"&Redo\tCtrl+Y");bar->Append(edit,"&Edit");
        if(!context_.embedded) {
            auto* view=new wxMenu;darkModeItem_=view->AppendCheckItem(ID_DarkMode,"&Dark Mode");
            view->AppendSeparator();view->Append(ID_FontIncrease,"Increase UI Font Size\tCtrl++");
            view->Append(ID_FontDecrease,"Decrease UI Font Size\tCtrl+-");view->Append(ID_FontReset,"Reset UI Font Size\tCtrl+0");bar->Append(view,"&View");
            auto* help=new wxMenu;help->Append(ID_About,"&About NeoGUI");bar->Append(help,"&Help");
            fontScaleWheelFilter_.attach(wxGetTopLevelParent(this),[this](int steps){changeFontScale(steps);});
        }
        setModuleMenus(bar);
        Bind(wxEVT_MENU,[this](wxCommandEvent& event){command(event.GetId());},ID_Open,ID_ClearRecent);
        Bind(wxEVT_MENU,[this](wxCommandEvent& event){openFile(settings_.recentFiles().at(std::size_t(event.GetId()-ID_RecentFirst)));},ID_RecentFirst,ID_RecentLast);
        Bind(wxEVT_MENU_OPEN,[this](wxMenuEvent& event){if(event.GetMenu()==recentFilesMenu_)refreshRecentFiles();event.Skip();});
        Bind(wxEVT_BUTTON,[this](wxCommandEvent& event){command(event.GetId());},ID_Open,ID_Redo);
        Bind(wxEVT_UPDATE_UI,[this](wxUpdateUIEvent& event){const auto id=event.GetId();if(id==ID_Save||id==ID_SaveAs)event.Enable(bool(model_));else if(id==ID_Undo)event.Enable(model_&&historyIndex_>0);else if(id==ID_Redo)event.Enable(model_&&historyIndex_+1<history_.size());},ID_Save,ID_Redo);
        tree_->Bind(wxEVT_TREE_SEL_CHANGED,[this](wxTreeEvent& event){if(rebuilding_)return;selected_=static_cast<ControlData*>(tree_->GetItemData(event.GetItem()))->index;canvas_->select(selected_);showProperties();});
        properties_->Bind(wxEVT_LIST_ITEM_ACTIVATED,[this](wxListEvent& event){editProperty(std::size_t(event.GetIndex()));});
        preview_->Bind(wxEVT_CHECKBOX,[this](wxCommandEvent&){try {canvas_->setAssets(preview_->GetValue());if(preview_->GetValue())cacheAssets();}catch(const std::exception& error){wxui::showError(this,error);}});
        game_->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){renderer_.setK2(game_->GetSelection()==1);canvas_->Refresh();});
        layoutSource_->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){refresh();});
        zoom->Bind(wxEVT_CHOICE,[this,zoom](wxCommandEvent&){canvas_->setZoom(std::array<double,4>{0,.5,1,2}.at(zoom->GetSelection()));});
        grid->Bind(wxEVT_CHOICE,[this,grid](wxCommandEvent&){canvas_->setGrid(std::array<int,4>{1,4,8,16}.at(grid->GetSelection()));});
        state->Bind(wxEVT_CHOICE,[this,state](wxCommandEvent&){canvas_->setState(state->GetSelection());});
        Bind(wxEVT_DPI_CHANGED,[this](wxDPIChangedEvent& event){CallAfter([this]{applyAppearance();});event.Skip();});
        setModuleStatusText("Open a .gui layout. Select Game assets to load its textures and fonts.");showProperties();refresh();applyAppearance();
    }
    ~Editor() override {DestroyChildren();}
    bool openFile(const std::filesystem::path& path) override {
        try {const auto absolute=std::filesystem::absolute(path);neoshared::ResourceDocument source{neoshared::pathToUtf8(absolute),neoshared::pathToUtf8(path.filename()),neoshared::pathToUtf8(path),2047,neoshared::texture::readFileBytes(path),{}};
            if(!load(std::move(source),absolute))return false;
            settings_.addRecentFile(absolute);refreshRecentFiles();return true;}
        catch(const std::exception& error){wxui::showError(this,error);return false;}
    }
    bool openResource(neoshared::ResourceDocument source) override {
        try {return load(std::move(source),{});}catch(const std::exception& error){wxui::showError(this,error);return false;}
    }
    bool canClose() override {
        if(!dirty())return true;
        const int answer=wxMessageBox("Save changes to this GUI layout?","Unsaved GUI layout",wxYES_NO|wxCANCEL|wxICON_QUESTION,this);
        return answer==wxNO||(answer==wxYES&&save(false));
    }
    void setAppearance(bool dark,double scale) override {darkMode_=dark;fontScale_=scale;applyAppearance();}
    std::vector<std::filesystem::path> openPaths() const override {
        auto result=source_.protectedInputs;result.insert(result.end(),assetInputs_.begin(),assetInputs_.end());if(!path_.empty())result.push_back(path_);if(model_&&model_->tlk().loaded())result.push_back(model_->tlk().filename());return result;
    }
private:
    void applyAppearance() {
        if(!context_.embedded)darkModeItem_->Check(darkMode_);
        wxui::applyTheme(this,darkMode_);neoview::applyFontScale(this,fontScale_);canvas_->Refresh();
    }
    void changeFontScale(int steps) {
        fontScale_=neoview::steppedFontScale(fontScale_,steps);settings_.setFontScale(fontScale_);neoview::applyFontScale(this,fontScale_);
    }
    void refreshRecentFiles() {neosettings::populateRecentFilesMenu(*recentFilesMenu_,settings_,ID_RecentFirst,ID_ClearRecent);}
    void chooseOpen(const std::filesystem::path& directory={}) {
        wxFileDialog dialog(this,"Open GUI layout",neosettings::pathToWx(directory),{},"GUI layouts (*.gui)|*.gui;*.GUI",wxFD_OPEN|wxFD_FILE_MUST_EXIST);
        if(dialog.ShowModal()==wxID_OK)openFile(neosettings::pathFromWx(dialog.GetPath()));
    }
    bool dirty() const {return model_&&history_.at(historyIndex_)!=saved_;}
    bool load(neoshared::ResourceDocument source,const std::filesystem::path& path) {
        auto candidate=std::make_unique<GffModel>();neoshared::loadGffResource(source,*candidate,"GUI");
        auto snapshot=candidate->gff().SaveBytes();if(!canClose())return false;
        model_=std::move(candidate);source_=std::move(source);path_=path;history_={snapshot};saved_=std::move(snapshot);historyIndex_=0;selected_=-1;
        layoutSource_->SetSelection(!path.empty()||source_.identity.find('#')==std::string::npos?1:0);
        textures_.clear();assetInputs_.clear();renderer_.clear();preview_->SetValue(false);canvas_->setAssets(false);
        if(context_.captureResourceResolver) {auto captured=context_.captureResourceResolver();resolver_=[captured=std::move(captured)](const std::string& name,std::uint16_t type){return captured(name,type,{});};}
        else if(context_.resolveResource)resolver_=context_.resolveResource;
        rebuildTree();showProperties();refresh();return true;
    }
    void refresh() {
        if(model_)renderer_.setLayout(path_.empty()?source_.fileName:neoshared::pathToUtf8(path_.filename()),layoutSource_->GetSelection()==1);
        canvas_->setDocument(model_?model_->gff().root():nullptr,selected_);
        setModuleTitle(wxui::toWx(std::string("NeoGUI — ")+(model_?(path_.empty()?source_.fileName:neoshared::pathToUtf8(path_.filename()))+(dirty()?" *":""):"GUI layout editor")));
        enableModuleCommand(ID_Assets,true);enableModuleCommand(ID_HostAssets,bool(context_.captureResourceResolver));enableModuleCommand(ID_Tlk,bool(model_));enableModuleCommand(ID_Save,bool(model_));enableModuleCommand(ID_SaveAs,bool(model_));enableModuleCommand(ID_Undo,model_&&historyIndex_>0);enableModuleCommand(ID_Redo,model_&&historyIndex_+1<history_.size());
        FindWindow(ID_Save)->Enable(bool(model_));FindWindow(ID_SaveAs)->Enable(bool(model_));FindWindow(ID_Undo)->Enable(model_&&historyIndex_>0);FindWindow(ID_Redo)->Enable(model_&&historyIndex_+1<history_.size());preview_->Enable(model_&&bool(resolver_));
        setModuleStatusText(model_?source_.sourceDescription+" | "+std::to_string(controls(*model_->gff().root()).count())+" controls | "+(dirty()?"Modified":"Saved"):"Open a GUI layout.");
    }
    void recordEdit() {
        auto snapshot=model_->gff().SaveBytes();
        if(snapshot!=history_.at(historyIndex_)) {history_.resize(historyIndex_+1);history_.push_back(std::move(snapshot));++historyIndex_;}
        rebuildTree();showProperties();refresh();
    }
    void undo(std::size_t index) {model_->gff().LoadBytes(history_.at(index));historyIndex_=index;rebuildTree();showProperties();refresh();if(preview_->GetValue())cacheAssets();}
    void rebuildTree() {
        rebuilding_=true;tree_->Freeze();tree_->DeleteAllItems();items_.clear();const auto root=tree_->AddRoot(wxui::toWx(source_.fileName),-1,-1,new ControlData(-1));items_.push_back(root);
        for(std::size_t i=0;i<controls(*model_->gff().root()).count();++i) {const auto& control=*controls(*model_->gff().root()).GetStruct(i);const auto label=string(control,"TAG")+"  ["+std::to_string(integer(control,"ID",-1))+"]";items_.push_back(tree_->AppendItem(root,wxui::toWx(label),-1,-1,new ControlData(int(i))));}
        tree_->Expand(root);tree_->SelectItem(items_.at(std::size_t(selected_+1)));tree_->Thaw();rebuilding_=false;
    }
    std::string prefix() const {return selected_<0?std::string{}:"CONTROLS\\"+std::to_string(selected_)+"\\";}
    const GffStruct& selectedRecord() const {return selected_<0?*model_->gff().root():*controls(*model_->gff().root()).GetStruct(std::size_t(selected_));}
    void showProperties() {
        properties_->DeleteAllItems();rows_.clear();apply_->Enable(bool(model_));all_->Enable(bool(model_));for(auto* box:geometry_)box->Enable(bool(model_));
        if(!model_)return;
        const auto rectangle=extent(selectedRecord());const std::array<double,4> dimensions={rectangle.m_x,rectangle.m_y,rectangle.m_width,rectangle.m_height};
        for(std::size_t i=0;i<geometry_.size();++i)geometry_[i]->ChangeValue(std::to_string(int(dimensions[i])));
        const auto selected=prefix();
        for(auto row:model_->rows())if(all_->GetValue()||(selected.empty()?row.path.compare(0,9,"CONTROLS\\")!=0:row.path.compare(0,selected.size(),selected)==0)) {
            const auto i=properties_->InsertItem(properties_->GetItemCount(),wxui::toWx(row.path));properties_->SetItem(i,1,wxui::toWx(row.type));properties_->SetItem(i,2,wxui::toWx(row.value));rows_.push_back(std::move(row));
        }
    }
    void editProperty(std::size_t index) {
        const auto row=rows_.at(index);if(!row.editable)return;
        wxTextEntryDialog dialog(this,wxui::toWx(row.path+" ("+row.type+")"),"Edit GUI field",wxui::toWx(row.value),wxOK|wxCANCEL|wxRESIZE_BORDER|wxTE_MULTILINE);
        wxui::applyTheme(&dialog,darkMode_);neoview::applyFontScale(&dialog,fontScale_);
        dialog.SetSize(FromDIP(wxSize(550,240)));if(dialog.ShowModal()!=wxID_OK)return;
        try {model_->setValue(row.path,wxui::toStd(dialog.GetValue()));recordEdit();if(preview_->GetValue())cacheAssets();}
        catch(const std::exception& error){wxui::showError(this,error);}
    }
    void editExtent(Rect r) {
        try {const auto base=prefix()+"EXTENT\\";const std::array<double,4> values={r.m_x,r.m_y,r.m_width,r.m_height};const char* labels[]={"LEFT","TOP","WIDTH","HEIGHT"};
            for(std::size_t i=0;i<values.size();++i)model_->setValue(base+labels[i],std::to_string(int(std::lround(values[i]))));
            recordEdit();}
        catch(const std::exception& error){wxui::showError(this,error);}
    }
    void applyExtent() {
        try {std::array<int,4> value;for(std::size_t i=0;i<value.size();++i)value[i]=neogff::ParseInt32Decimal(wxui::toStd(geometry_[i]->GetValue()));editExtent({double(value[0]),double(value[1]),double(value[2]),double(value[3])});}
        catch(const std::exception& error){wxui::showError(this,error);}
    }
    bool save(bool choose) {
        try {auto output=path_;
            if(choose||output.empty()) {wxFileDialog dialog(this,"Save GUI layout",{},wxui::toWx(source_.fileName),"GUI layouts (*.gui)|*.gui;*.GUI",wxFD_SAVE|wxFD_OVERWRITE_PROMPT);if(dialog.ShowModal()!=wxID_OK)return false;output=neosettings::pathFromWx(dialog.GetPath());}
            neoshared::checkGffOutputType(output,".gui");auto inputs=source_.protectedInputs;inputs.insert(inputs.end(),assetInputs_.begin(),assetInputs_.end());if(model_->tlk().loaded())inputs.push_back(model_->tlk().filename());
            neoshared::checkResourceOutput(output,inputs);validateHostOutput(output);model_->gff().SaveFile(output);path_=std::filesystem::absolute(output);saved_=history_.at(historyIndex_);settings_.addRecentFile(path_);refreshRecentFiles();tree_->SetItemText(items_.front(),neosettings::pathToWx(path_.filename()));refresh();return true;
        } catch(const std::exception& error){wxui::showError(this,error);return false;}
    }
    const neoshared::texture::TextureData& texture(const std::string& name) {
        const auto key=neoshared::material::textureResourceKey(name);if(const auto found=textures_.find(key);found!=textures_.end())return found->second;
        for(const auto& [type,extension]:textureTypes)if(auto source=resolver_(key,type)) {
            std::string txi;if(type==3||type==2033)if(auto sidecar=resolver_(key,2022)) {txi.assign(sidecar->bytes.begin(),sidecar->bytes.end());assetInputs_.insert(assetInputs_.end(),sidecar->protectedInputs.begin(),sidecar->protectedInputs.end());}
            auto image=neoshared::texture::decodeTextureBytes(source->bytes,neoshared::pathFromUtf8(key+extension),std::move(txi));assetInputs_.insert(assetInputs_.end(),source->protectedInputs.begin(),source->protectedInputs.end());return textures_.emplace(key,std::move(image)).first->second;
        }
        throw std::runtime_error("Missing GUI texture/font: "+name);
    }
    void loadAssets(const GffStruct& record) {
        if(const auto* text=field<GffStruct>(record,"TEXT")) {const auto font=string(*text,"FONT");texture(font.empty()?"dialogfont16x16":font);}
        for(const auto& value:record.allFields()) {
            if(const auto* child=dynamic_cast<const GffStruct*>(value.get()))loadAssets(*child);
            else if(const auto* list=dynamic_cast<const GffList*>(value.get()))for(const auto& child:list->allStructs())loadAssets(*child);
            else {
                const auto label=value->GetLabel();
                if(label=="CORNER"||label=="EDGE"||label=="FILL"||label=="IMAGE"||label=="BACKGROUND") {const auto name=value->GetString();if(!name.empty())texture(name);}
            }
        }
    }
    void cacheAssets() {renderer_.setLanguage(model_->tlk().languageId());loadAssets(*model_->gff().root());renderer_.cacheControl(*model_->gff().root());for(const auto& control:controls(*model_->gff().root()).allStructs())renderer_.cacheControl(*control);canvas_->Refresh();}
    void assetFolder() {
        wxDirDialog dialog(this,"Choose extracted GUI textures and fonts");if(dialog.ShowModal()!=wxID_OK)return;
        const auto directory=neosettings::pathFromWx(dialog.GetPath());auto files=std::make_shared<std::map<Key,std::filesystem::path>>();
        for(const auto& entry:std::filesystem::directory_iterator(directory)) {
            auto extension=neoshared::pathToUtf8(entry.path().extension());std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return char(std::tolower(c));});
            for(const auto& [type,suffix]:textureTypes)if(extension==suffix)(*files)[{neoshared::material::textureResourceKey(neoshared::pathToUtf8(entry.path().stem())),type}]=entry.path();
            if(extension==".txi")(*files)[{neoshared::material::textureResourceKey(neoshared::pathToUtf8(entry.path().stem())),2022}]=entry.path();
        }
        resolver_=[files](const std::string& name,std::uint16_t type)->std::optional<neoshared::ResourceDocument> {
            const auto file=files->find({name,type});if(file==files->end())return {};
            const auto& path=file->second;return neoshared::ResourceDocument{neoshared::pathToUtf8(path),neoshared::pathToUtf8(path.filename()),neoshared::pathToUtf8(path),type,neoshared::texture::readFileBytes(path),{path}};
        };resetAssets();
    }
    void resetAssets() {preview_->SetValue(false);canvas_->setAssets(false);textures_.clear();assetInputs_.clear();renderer_.clear();refresh();}
    void command(int id) {
        try {switch(id) {
            case ID_Open:chooseOpen();break;
            case ID_Save:save(false);break;case ID_SaveAs:save(true);break;
            case ID_Undo:undo(historyIndex_-1);break;case ID_Redo:undo(historyIndex_+1);break;
            case ID_Assets:assetFolder();break;
            case ID_HostAssets: {auto captured=context_.captureResourceResolver();resolver_=[captured=std::move(captured)](const std::string& name,std::uint16_t type){return captured(name,type,{});};resetAssets();break;}
            case ID_Tlk: {wxFileDialog dialog(this,"Load GUI talk table",{},{},"Talk tables (*.tlk)|*.tlk;*.TLK",wxFD_OPEN|wxFD_FILE_MUST_EXIST);if(dialog.ShowModal()==wxID_OK) {model_->loadTlk(neosettings::pathFromWx(dialog.GetPath()));showProperties();canvas_->Refresh();}break;}
            case ID_DarkMode:darkMode_=darkModeItem_->IsChecked();settings_.setDarkMode(darkMode_);applyAppearance();break;
            case ID_FontIncrease:fontScaleWheelFilter_.reset();changeFontScale(1);break;
            case ID_FontDecrease:fontScaleWheelFilter_.reset();changeFontScale(-1);break;
            case ID_FontReset:fontScaleWheelFilter_.reset();fontScale_=neoview::kDefaultFontScale;settings_.setFontScale(fontScale_);neoview::applyFontScale(this,fontScale_);break;
            case ID_ClearRecent:settings_.clearRecentFiles();refreshRecentFiles();break;
            case ID_Exit:requestModuleClose();break;
            case ID_About:wxMessageBox("NeoGUI\nEdit GUI layouts and preview game textures, fonts and control states.","About NeoGUI",wxOK|wxICON_INFORMATION,this);break;
        }}catch(const std::exception& error){wxui::showError(this,error);}
    }
    std::unique_ptr<GffModel> model_;
    neosettings::AppSettings settings_{"NeoGUI"};
    neoview::FontScaleWheelFilter fontScaleWheelFilter_;
    std::unique_ptr<neogames::OpenGameDirectoryMenu> gameDirectoryMenu_;
    wxMenu* recentFilesMenu_{};
    wxMenuItem* darkModeItem_{};
    bool darkMode_{};
    double fontScale_{neoview::kDefaultFontScale};
    neoshared::ResourceDocument source_;
    std::filesystem::path path_;
    Resolver resolver_;
    std::map<std::string,neoshared::texture::TextureData> textures_;
    std::vector<std::filesystem::path> assetInputs_;
    std::vector<std::vector<std::uint8_t>> history_;
    std::vector<std::uint8_t> saved_;
    std::size_t historyIndex_{};
    GuiRenderer renderer_;
    Canvas* canvas_{};
    wxTreeCtrl* tree_{};
    wxListCtrl* properties_{};
    wxChoice* game_{},*layoutSource_{};
    wxCheckBox* preview_{};wxCheckBox* all_{};
    wxButton* apply_{};
    std::array<wxTextCtrl*,4> geometry_{};
    std::vector<wxTreeItemId> items_;
    std::vector<GffFieldRow> rows_;
    int selected_{-1};bool rebuilding_{};
};
}
GUIEditorPanel* createEditorPanel(wxWindow* parent,neomodules::Context context) {return new Editor(parent,std::move(context));}
}
