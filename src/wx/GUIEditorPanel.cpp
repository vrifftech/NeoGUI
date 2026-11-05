#include "wx/GUIEditorPanel.hpp"
#include "GuiRenderer.hpp"
#include "GameResources.hpp"
#include <neoshared/bif/ModFolder.hpp>
#include "NeoSettings.hpp"
#include "NeoViewState.hpp"
#include "NeoDocumentTabs.hpp"
#include "NeoGameDirectoryMenu.hpp"
#include "NeoPatcherExport.hpp"
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
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dirdlg.h>
#include <wx/dnd.h>
#include <wx/filedlg.h>
#include <wx/listctrl.h>
#include <wx/choicdlg.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>
#include <wx/treectrl.h>
#include <wx/wrapsizer.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace neogui::ui {
namespace {
enum {ID_Open=wxID_HIGHEST+25000,ID_Save,ID_SaveAs,ID_Undo,ID_Redo,ID_Assets,ID_HostAssets,ID_Tlk,
      ID_DarkMode,ID_FontIncrease,ID_FontDecrease,ID_FontReset,ID_Exit,ID_About,ID_ClearRecent,
      ID_New,ID_Close,ID_CloseOthers,ID_Next,ID_Previous,ID_Find,ID_FindNext,ID_Copy,ID_Paste,
      ID_Duplicate,ID_Delete,ID_ImportControl,ID_Front,ID_Back,ID_CopyValue,ID_GeneratePatcher,
      ID_EditValue,ID_RevertGeometry,
      ID_RecentFirst,ID_RecentLast=ID_RecentFirst+neosettings::kMaxRecentFiles-1};
using Resolver=neoshared::ResourceResolver;
using Key=std::pair<std::string,std::uint16_t>;
constexpr std::pair<std::uint16_t,const char*> textureTypes[]={{3007,".tpc"},{3006,".txb"},{3,".tga"},{2033,".dds"}};

std::optional<neogames::GameInstall> guiInstallation(const std::filesystem::path& path,int selectedGame) {
    const auto& settings=neogames::resolver().settings();
    auto directory=path.parent_path();
    while(!directory.empty()) {
        if(neogames::isRegularFilePath(directory/"chitin.key")) {
            auto local=neoshared::game::inspectGameInstallation(directory);
            if(local&&(local->id=="kotor"||local->id=="kotor2")) {
                for(const auto& saved:settings.readSaved(*neogames::findGame(local->id)))
                    if(neosettings::samePathForMru(saved.installPath,local->installPath))return saved;
                return local;
            }
        }
        const auto parent=directory.parent_path();
        if(parent==directory)break;
        directory=parent;
    }
    return settings.read(*neogames::findGame(selectedGame==1?"kotor2":"kotor"));
}

class Canvas final : public wxScrolledWindow {
public:
    Canvas(wxWindow* parent,GuiRenderer& renderer,const std::function<const neotlk::TlkLookup&()>& tlk,
           std::function<bool()> commit,std::function<void(int)> select,std::function<void(Rect)> edit)
        :wxScrolledWindow(parent),renderer_(renderer),tlk_(tlk),commit_(std::move(commit)),select_(std::move(select)),edit_(std::move(edit)) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);SetMinSize(FromDIP(wxSize(300,250)));
        SetScrollRate(16,16);
        SetToolTip("Click to select; drag to move or resize. Arrow keys move by the grid step; Shift moves ten steps. Esc cancels a drag.");
        Bind(wxEVT_PAINT,&Canvas::paint,this);
        Bind(wxEVT_SIZE,[this](wxSizeEvent& event){updateViewport();event.Skip();});
        Bind(wxEVT_LEFT_DOWN,&Canvas::down,this);Bind(wxEVT_MOTION,&Canvas::motion,this);
        Bind(wxEVT_LEFT_UP,[this](wxMouseEvent&){
            if(!gesture_)return;
            const auto result=*gesture_;gesture_.reset();ReleaseMouse();Refresh();
            if(result!=original_)edit_(result);
        });
        Bind(wxEVT_MOUSE_CAPTURE_LOST,[this](wxMouseCaptureLostEvent&){gesture_.reset();Refresh();});
        Bind(wxEVT_CHAR_HOOK,[this](wxKeyEvent& event){
            if(event.GetKeyCode()==WXK_ESCAPE&&gesture_) {gesture_.reset();ReleaseMouse();Refresh();}
            else if(!gesture_&&selected_>=0&&(event.GetKeyCode()==WXK_LEFT||event.GetKeyCode()==WXK_RIGHT||event.GetKeyCode()==WXK_UP||event.GetKeyCode()==WXK_DOWN)) {
                auto r=rect(selected_);const int step=grid_*(event.ShiftDown()?10:1);
                if(event.GetKeyCode()==WXK_LEFT)r.m_x-=step;
                if(event.GetKeyCode()==WXK_RIGHT)r.m_x+=step;
                if(event.GetKeyCode()==WXK_UP)r.m_y-=step;
                if(event.GetKeyCode()==WXK_DOWN)r.m_y+=step;
                edit_(r);
            } else event.Skip();
        });
    }
    void setDocument(const GffStruct* root,int selected) {if(HasCapture())ReleaseMouse();root_=root;selected_=selected;gesture_.reset();updateViewport();}
    void select(int selected) {selected_=selected;Refresh();}
    void setAssets(bool value) {assets_=value;Refresh();}
    void setZoom(double value) {zoom_=value;updateViewport();}
    void setGrid(int value) {grid_=value;}
    void setState(int value) {state_=value;Refresh();}
    void setBounds(bool value) {bounds_=value;Refresh();}
private:
    void updateViewport() {
        const auto size=GetClientSize();
        if(root_) {
            const auto r=extent(*root_);
            scale_=zoom_?zoom_:std::min((size.x-24)/r.m_width,(size.y-24)/r.m_height);
            const wxSize area=zoom_?wxSize(std::max(size.x,int(std::ceil(r.m_width*scale_))+24),std::max(size.y,int(std::ceil(r.m_height*scale_))+24)):size;
            offsetX_=(area.x-r.m_width*scale_)/2;offsetY_=(area.y-r.m_height*scale_)/2;SetVirtualSize(area);
        } else SetVirtualSize(size);
        Refresh();
    }
    Rect rect(int index) const {return index==selected_&&gesture_?*gesture_:extent(*controls(*root_).GetStruct(std::size_t(index)));}
    wxPoint2DDouble point(wxPoint value) const {const auto p=CalcUnscrolledPosition(value);return {(p.x-offsetX_)/scale_,(p.y-offsetY_)/scale_};}
    void down(wxMouseEvent& event) {
        if(!root_||!commit_())return;
        SetFocus();const auto p=point(event.GetPosition());int hit=-1;
        if(selected_>=0) {const auto r=rect(selected_);resizing_=std::abs(p.m_x-r.GetRight())<=7/scale_&&std::abs(p.m_y-r.GetBottom())<=7/scale_;if(resizing_)hit=selected_;}
        if(hit<0) {resizing_=false;for(std::size_t i=controls(*root_).count();i>0;--i)if(rect(int(i-1)).Contains(p)) {hit=int(i-1);break;}}
        select_(hit);
        selected_=hit;Refresh();
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
        wxAutoBufferedPaintDC paint(this);DoPrepareDC(paint);paint.SetBackground(wxBrush(GetBackgroundColour()));paint.Clear();
        const auto drawError=[&](const wxString& message) {paint.SetTextForeground(wxui::currentDarkMode()?wxColour(255,160,140):wxColour(170,35,25));paint.DrawText(message,CalcUnscrolledPosition(FromDIP(wxPoint(12,12))));};
        if(!root_) {
            paint.SetTextForeground(GetForegroundColour());int y=FromDIP(14);
            for(const auto* line:{"New or Open a GUI layout.","Add Control, or select a control.","Edit geometry and fields on the right.","Choose Assets for preview resources.","Save the GUI or generate an Installer INI."}) {paint.DrawText(line,FromDIP(14),y);y+=paint.GetCharHeight()+FromDIP(6);}return;
        }
        try {
            const auto root=extent(*root_);renderer_.setViewport(renderer_.layoutExtent({0,0,root.m_width,root.m_height}));
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
            for(std::size_t i=0;i<controls(*root_).count();++i)if(bounds_||int(i)==selected_) {
                const auto r=rect(int(i));dc->SetPen(wxPen(int(i)==selected_?wxColour(255,205,70):wxColour(100,150,185),1/scale_));
                dc->DrawRectangle(r.m_x,r.m_y,r.m_width,r.m_height);
            }
            if(selected_>=0) {const auto r=rect(selected_);dc->SetBrush(wxBrush(wxColour(255,205,70)));dc->DrawRectangle(r.GetRight()-4/scale_,r.GetBottom()-4/scale_,8/scale_,8/scale_);}
        } catch(const std::bad_optional_access&) {drawError("Unresolved text reference. Load a matching talk table from Assets.");}
        catch(const std::exception& error) {drawError(wxui::toWx(error.what()));}
    }
    GuiRenderer& renderer_;
    std::function<const neotlk::TlkLookup&()> tlk_;
    std::function<bool()> commit_;
    std::function<void(int)> select_;
    std::function<void(Rect)> edit_;
    const GffStruct* root_{};
    int selected_{-1},grid_{1},state_{};
    bool assets_{},resizing_{},bounds_{true};
    double scale_{1},zoom_{},offsetX_{},offsetY_{};
    wxPoint2DDouble start_;
    Rect original_;
    std::optional<Rect> gesture_;
};
class ControlData final : public wxTreeItemData {
public:explicit ControlData(int value):index(value) {} int index;
};
class FileDrop final : public wxFileDropTarget {
public:
    explicit FileDrop(std::function<bool(const std::filesystem::path&)> open):open_(std::move(open)) {}
    bool OnDropFiles(wxCoord,wxCoord,const wxArrayString& files) override {
        bool opened=false;for(const auto& file:files)opened=open_(neosettings::pathFromWx(file))||opened;return opened;
    }
private:std::function<bool(const std::filesystem::path&)> open_;
};
struct Snapshot {std::vector<std::uint8_t> bytes;int selected{-1};std::string fieldPath;};
struct Document {
    std::unique_ptr<GffModel> model;
    neoshared::ResourceDocument source;
    std::filesystem::path path;
    std::string assets,fieldPath,findText,findPath;
    Resolver resolver;
    bool hostAssets{};
    std::optional<neogames::GameInstall> installation;
    std::set<std::filesystem::path> assetInputs;
    std::map<std::string,neoshared::texture::TextureData> textures;
    std::vector<Snapshot> history;
    std::vector<std::uint8_t> saved;
    std::size_t historyIndex{};
    int selected{-1},game{},layout{},zoom{},grid{},state{};
    bool preview{},allFields{},bounds{true};
    wxPoint scroll;
    wxWindow* tabPage{};
};
class Editor final : public GUIEditorPanel {
public:
    Editor(wxWindow* parent,neomodules::Context context):GUIEditorPanel(parent,std::move(context)),renderer_([this](const auto& name)->const auto&{return texture(name);}) {
        darkMode_=settings_.darkMode();fontScale_=settings_.fontScale();
        auto* box=new wxBoxSizer(wxVERTICAL);auto* toolbar=new wxWrapSizer(wxHORIZONTAL,wxREMOVE_LEADING_SPACES);
        tabs_=new wxAuiNotebook(this,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxAUI_NB_TOP|wxAUI_NB_TAB_MOVE|wxAUI_NB_CLOSE_ON_ACTIVE_TAB|wxAUI_NB_SCROLL_BUTTONS);
        box->Add(tabs_,0,wxEXPAND);document().tabPage=neotabs::addTabPage(tabs_,"GUI layout",false);
        const auto button=[&](int id,const char* title){auto* control=new wxButton(this,id,title);toolbar->Add(control,0,wxALL,4);};
        button(ID_New,"New");button(ID_Open,"Open...");button(ID_Save,"Save...");button(ID_SaveAs,"Save As...");button(ID_GeneratePatcher,"Installer INI...");
        toolbar->AddSpacer(8);button(ID_Undo,"Undo");button(ID_Redo,"Redo");box->Add(toolbar,0,wxEXPAND);
        auto* previewBar=new wxWrapSizer(wxHORIZONTAL);
        preview_=new wxCheckBox(this,wxID_ANY,"Render game assets");previewBar->Add(preview_,0,wxALIGN_CENTER_VERTICAL|wxALL,4);
        const auto choice=[&](const char* label,std::initializer_list<const char*> values) {
            auto* group=new wxBoxSizer(wxHORIZONTAL);group->Add(new wxStaticText(this,wxID_ANY,label),0,wxALIGN_CENTER_VERTICAL|wxRIGHT,4);
            auto* control=new wxChoice(this,wxID_ANY);for(const auto* value:values)control->Append(value);control->SetSelection(0);
            group->Add(control);previewBar->Add(group,0,wxALL,4);return control;
        };
        game_=choice("Preview rules:",{"KOTOR 1","KOTOR 2"});
        const auto activeGame=neogames::resolver().settings().activeGameId();game_->SetSelection(activeGame&&*activeGame=="kotor2"?1:0);
        layoutSource_=choice("K1 scaling:",{"Archive","Loose / Override"});
        zoom_=choice("Zoom:",{"Fit","50%","100%","200%"});grid_=choice("Snap:",{"1 px","4 px","8 px","16 px"});
        state_=choice("Preview state:",{"Normal","Highlight","Selected","Selected + highlight"});
        bounds_=new wxCheckBox(this,wxID_ANY,"Control bounds");bounds_->SetValue(true);previewBar->Add(bounds_,0,wxALIGN_CENTER_VERTICAL|wxALL,4);
        preview_->SetToolTip("Render textures and fonts from the asset source shown below. Choose a different source in Assets.");
        game_->SetToolTip("Rendering rules and the saved default asset directory for layouts opened outside a game folder.");
        layoutSource_->SetToolTip("KOTOR 1 archive and loose-file scaling. KOTOR 2 does not use this setting.");
        zoom_->SetToolTip("Fixed zoom supports horizontal and vertical scrolling.");
        grid_->SetToolTip("Move and resize in these pixel increments. Shift + arrow keys moves ten increments.");
        state_->SetToolTip("Preview highlight and selection states without changing control fields.");
        bounds_->SetToolTip("Show control outlines. The selected control stays marked.");
        box->Add(previewBar,0,wxEXPAND);
        assetInfo_=new wxStaticText(this,wxID_ANY,{},wxDefaultPosition,wxDefaultSize,wxST_ELLIPSIZE_MIDDLE);assetInfo_->SetMinSize(wxSize(0,-1));box->Add(assetInfo_,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,6);
        auto* split=new wxSplitterWindow(this,wxID_ANY);auto* controlPane=new wxPanel(split);auto* controlBox=new wxBoxSizer(wxVERTICAL);auto* controlActions=new wxWrapSizer(wxHORIZONTAL);
        for(const auto& [id,label]:std::initializer_list<std::pair<int,const char*>>{{ID_ImportControl,"Add Control..."},{ID_Duplicate,"Duplicate"},{ID_Delete,"Delete"}})controlActions->Add(new wxButton(controlPane,id,label),0,wxALL,4);
        controlBox->Add(controlActions,0,wxEXPAND);tree_=new wxTreeCtrl(controlPane,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxTR_HAS_BUTTONS|wxTR_SINGLE);controlBox->Add(tree_,1,wxEXPAND);controlPane->SetSizer(controlBox);
        auto* right=new wxSplitterWindow(split,wxID_ANY);canvas_=new Canvas(right,renderer_,[this]()->const auto&{return document().model->tlk();},[this]{return commitGeometry();},[this](int index){tree_->SelectItem(items_.at(std::size_t(index+1)));},[this](Rect r){editExtent(r);});
        auto* inspector=new wxScrolledWindow(right);inspector->SetScrollRate(8,8);auto* props=new wxBoxSizer(wxVERTICAL);auto* dimensions=new wxFlexGridSizer(0,4,4,4);
        selectionInfo_=new wxStaticText(inspector,wxID_ANY,{},wxDefaultPosition,wxDefaultSize,wxST_ELLIPSIZE_MIDDLE);selectionInfo_->SetMinSize(wxSize(0,-1));props->Add(selectionInfo_,0,wxEXPAND|wxALL,6);
        for(std::size_t i=0;i<geometry_.size();++i) {dimensions->Add(new wxStaticText(inspector,wxID_ANY,std::array<const char*,4>{"Left","Top","Width","Height"}[i]),0,wxALIGN_CENTER_VERTICAL);geometry_[i]=new wxTextCtrl(inspector,wxID_ANY,{},wxDefaultPosition,FromDIP(wxSize(90,-1)),wxTE_PROCESS_ENTER);dimensions->Add(geometry_[i],1,wxEXPAND);geometry_[i]->Bind(wxEVT_TEXT_ENTER,[this](wxCommandEvent&){applyExtent();});geometry_[i]->Bind(wxEVT_TEXT,[this](wxCommandEvent&){geometryDirty_=true;refresh();});}
        dimensions->AddGrowableCol(1);dimensions->AddGrowableCol(3);props->Add(dimensions,0,wxEXPAND|wxALL,6);
        auto* geometryActions=new wxWrapSizer(wxHORIZONTAL);
        apply_=new wxButton(inspector,wxID_ANY,"Apply geometry");geometryActions->Add(apply_,0,wxRIGHT|wxBOTTOM,6);apply_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){applyExtent();});
        revert_=new wxButton(inspector,ID_RevertGeometry,"Revert geometry");geometryActions->Add(revert_,0,wxBOTTOM,6);props->Add(geometryActions,0,wxEXPAND|wxLEFT|wxRIGHT,6);
        all_=new wxCheckBox(inspector,wxID_ANY,"Show all layout fields");props->Add(all_,0,wxALL,6);all_->Bind(wxEVT_CHECKBOX,[this](wxCommandEvent&){if(commitGeometry())showProperties();else all_->SetValue(document().allFields);});
        all_->SetToolTip("Include fields from every control. Geometry always belongs to the selection named above.");
        auto* fieldActions=new wxWrapSizer(wxHORIZONTAL);auto* editValue=new wxButton(inspector,ID_EditValue,"Edit value...");auto* copyValue=new wxButton(inspector,ID_CopyValue,"Copy value");fieldActions->Add(editValue,0,wxRIGHT,6);fieldActions->Add(copyValue);props->Add(fieldActions,0,wxLEFT|wxRIGHT|wxBOTTOM,6);
        properties_=new wxListCtrl(inspector,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxLC_REPORT|wxLC_SINGLE_SEL);
        properties_->InsertColumn(0,"Field",wxLIST_FORMAT_LEFT,FromDIP(170));properties_->InsertColumn(1,"Type",wxLIST_FORMAT_LEFT,FromDIP(90));properties_->InsertColumn(2,"Value",wxLIST_FORMAT_LEFT,FromDIP(220));properties_->InsertColumn(3,"Resolved text",wxLIST_FORMAT_LEFT,FromDIP(260));props->Add(properties_,1,wxEXPAND|wxALL,6);
        auto* note=new wxStaticText(inspector,wxID_ANY,"Enter/Apply commits geometry; Revert discards pending entries. Preview settings affect rendering only.");note->Wrap(FromDIP(320));props->Add(note,0,wxEXPAND|wxALL,6);inspector->SetSizer(props);
        right->SetMinimumPaneSize(FromDIP(300));right->SetSashGravity(0.7);right->SplitVertically(canvas_,inspector,-FromDIP(360));
        split->SetMinimumPaneSize(FromDIP(150));split->SplitVertically(controlPane,right,FromDIP(210));box->Add(split,1,wxEXPAND);SetSizer(box);createModuleStatusBar(1);
        auto* bar=new wxMenuBar;auto* file=new wxMenu;
        file->Append(ID_New,"&New GUI\tCtrl+N");
        file->Append(ID_Open,context_.embedded?"&Open GUI...\tCtrl+Shift+O":"&Open GUI...\tCtrl+O");
        recentFilesMenu_=new wxMenu;refreshRecentFiles();file->AppendSubMenu(recentFilesMenu_,"Open &Recent");
        saveItem_=file->Append(ID_Save,"&Save...\tCtrl+S");file->Append(ID_SaveAs,"Save &As...\tCtrl+Shift+S");
        file->AppendSeparator();file->Append(ID_Close,"&Close Tab\tCtrl+W");file->Append(ID_CloseOthers,"Close &Other Tabs");
        file->Append(ID_Next,"Next Tab\tCtrl+Tab");file->Append(ID_Previous,"Previous Tab\tCtrl+Shift+Tab");
        if(!context_.embedded) {file->AppendSeparator();file->Append(ID_Exit,"E&xit");}
        bar->Append(file,"&File");
        auto* edit=new wxMenu;edit->Append(ID_Undo,"&Undo\tCtrl+Z");edit->Append(ID_Redo,"&Redo\tCtrl+Y");
        edit->AppendSeparator();edit->Append(ID_EditValue,"Edit Field Value...\tF2");edit->Append(ID_CopyValue,"Copy Field Value");
        edit->Append(ID_Find,"&Find control or field...\tCtrl+F");edit->Append(ID_FindNext,"Find &Next\tF3");
        edit->AppendSeparator();edit->Append(ID_ImportControl,"Add Control from Layout...");
        edit->Append(ID_Copy,"Copy Control\tCtrl+Shift+C");edit->Append(ID_Paste,"Paste Control\tCtrl+Shift+V");
        edit->Append(ID_Duplicate,"Duplicate Control\tCtrl+D");edit->Append(ID_Delete,"Delete Control\tCtrl+Shift+Delete");
        edit->Append(ID_Front,"Bring Control to Front");edit->Append(ID_Back,"Send Control to Back");bar->Append(edit,"&Edit");
        auto* assets=new wxMenu;
        gameDirectoryMenu_=neogames::appendOpenGameDirectoryMenu(*this,*assets,
            neogames::OpenGameDirectoryAction{[this](const neogames::SavedGameDirectory& directory) {
                if(directory.customDirectory)assetFolder(directory.path);
                else {
                    const auto installs=neogames::resolver().settings().readSaved(*neogames::findGame(directory.gameId));
                    useGame(*std::find_if(installs.begin(),installs.end(),[&](const auto& install){return install.installId==directory.installId;}));
                }
            }},neogames::GameDirectoryGameIds{"kotor","kotor2"},"Saved game installation");
        assets->Append(ID_Assets,"Extracted textures/fonts folder...");
        if(context_.captureResourceResolver)assets->Append(ID_HostAssets,"Use game-browser assets");
        assets->AppendSeparator();assets->Append(ID_Tlk,"Load talk table (.tlk)...");bar->Append(assets,"&Assets");
        auto* tools=new wxMenu;tools->Append(ID_GeneratePatcher,"Generate TSLPatcher/HoloPatcher Instructions...");bar->Append(tools,"&Tools");
        if(!context_.embedded) {
            auto* view=new wxMenu;darkModeItem_=view->AppendCheckItem(ID_DarkMode,"&Dark Mode");
            view->AppendSeparator();view->Append(ID_FontIncrease,"Increase UI Font Size\tCtrl++");
            view->Append(ID_FontDecrease,"Decrease UI Font Size\tCtrl+-");view->Append(ID_FontReset,"Reset UI Font Size\tCtrl+0");bar->Append(view,"&View");
            auto* help=new wxMenu;help->Append(ID_About,"&About NeoGUI");bar->Append(help,"&Help");
            fontScaleWheelFilter_.attach(wxGetTopLevelParent(this),[this](int steps){changeFontScale(steps);});
        }
        setModuleMenus(bar);
        Bind(wxEVT_MENU,[this](wxCommandEvent& event){command(event.GetId());},ID_Open,ID_RevertGeometry);
        Bind(wxEVT_MENU,[this](wxCommandEvent& event){openFile(settings_.recentFiles().at(std::size_t(event.GetId()-ID_RecentFirst)));},ID_RecentFirst,ID_RecentLast);
        Bind(wxEVT_MENU_OPEN,[this,edit](wxMenuEvent& event){if(event.GetMenu()==recentFilesMenu_)refreshRecentFiles();if(event.GetMenu()==edit) {wxClipboardLocker clipboard;enableModuleCommand(ID_Paste,bool(document().model)&&wxTheClipboard->IsOpened()&&wxTheClipboard->IsSupported(wxDataFormat("NeoGUI.Control")));}event.Skip();});
        Bind(wxEVT_BUTTON,[this](wxCommandEvent& event){command(event.GetId());},ID_Open,ID_RevertGeometry);
        tree_->Bind(wxEVT_TREE_SEL_CHANGING,[this](wxTreeEvent& event){if(!rebuilding_&&!commitGeometry())event.Veto();});
        tree_->Bind(wxEVT_TREE_SEL_CHANGED,[this](wxTreeEvent& event){if(rebuilding_)return;const auto selected=static_cast<ControlData*>(tree_->GetItemData(event.GetItem()))->index;if(selected!=document().selected)document().fieldPath.clear();document().selected=selected;auto& snapshot=document().history.at(document().historyIndex);snapshot.selected=selected;snapshot.fieldPath=document().fieldPath;canvas_->select(selected);showProperties();});
        properties_->Bind(wxEVT_LIST_ITEM_ACTIVATED,[this](wxListEvent& event){editProperty(std::size_t(event.GetIndex()));});
        properties_->Bind(wxEVT_LIST_ITEM_SELECTED,[this](wxListEvent& event){document().fieldPath=rows_.at(std::size_t(event.GetIndex())).path;document().history.at(document().historyIndex).fieldPath=document().fieldPath;refreshCommands();});
        properties_->Bind(wxEVT_LIST_ITEM_DESELECTED,[this](wxListEvent&){refreshCommands();});
        preview_->Bind(wxEVT_CHECKBOX,[this](wxCommandEvent&){updatePreview();});
        game_->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){renderer_.setK2(game_->GetSelection()==1);refresh();});
        layoutSource_->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){refresh();});
        zoom_->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){canvas_->setZoom(std::array<double,4>{0,.5,1,2}.at(zoom_->GetSelection()));});
        grid_->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){canvas_->setGrid(std::array<int,4>{1,4,8,16}.at(grid_->GetSelection()));});
        state_->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){canvas_->setState(state_->GetSelection());});
        bounds_->Bind(wxEVT_CHECKBOX,[this](wxCommandEvent&){canvas_->setBounds(bounds_->GetValue());});
        tabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGING,[this](wxAuiNotebookEvent& event){if(!switching_&&!commitGeometry())event.Veto();});
        tabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED,[this](wxAuiNotebookEvent& event){if(!switching_) {rememberView();showDocument(indexOf(neotabs::pageForIndex(tabs_,event.GetSelection())));}event.Skip();});
        tabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE,[this](wxAuiNotebookEvent& event){event.Veto();closeDocument(indexOf(neotabs::pageForIndex(tabs_,event.GetSelection())));});
        for(auto* window:std::array<wxWindow*,4>{this,tree_,canvas_,properties_})window->SetDropTarget(new FileDrop([this](const auto& path){return openFile(path);}));
        Bind(wxEVT_DPI_CHANGED,[this](wxDPIChangedEvent& event){CallAfter([this]{applyAppearance();});event.Skip();});
        for(const auto& [id,tip]:std::initializer_list<std::pair<int,const char*>>{
            {ID_New,"Create an empty layout. Add Control copies a native control from another GUI."},
            {ID_Open,"Open one or more .gui files in separate tabs."},
            {ID_Save,"Save this layout to a .gui file. Archive resources require a working copy."},
            {ID_SaveAs,"Save this layout under a different filename."},
            {ID_GeneratePatcher,"Compare with a clean GUI and generate or merge TSLPatcher/HoloPatcher instructions."},
            {ID_ImportControl,"Choose a .gui file, then choose the native control to add."},
            {ID_Duplicate,"Copy the selected control with a unique tag/ID and a small position offset."},
            {ID_Delete,"Delete the selected control. Undo restores it."}})FindWindow(id)->SetToolTip(tip);
        setModuleStatusText("Open a .gui layout. Select Game assets to load its textures and fonts.");showProperties();refresh();applyAppearance();
    }
    ~Editor() override {DestroyChildren();}
    bool openFile(const std::filesystem::path& path) override {
        try {if(!commitGeometry())return false;
            const auto absolute=neosettings::storedPath(path);
            for(std::size_t i=0;i<documents_.size();++i)if(!documents_[i].path.empty()&&neosettings::samePathForMru(documents_[i].path,absolute)) {rememberView();showDocument(i);return true;}
            neoshared::ResourceDocument source{neoshared::pathToUtf8(absolute),neoshared::pathToUtf8(path.filename()),neoshared::pathToUtf8(path),2047,neoshared::texture::readFileBytes(path),{}};
            if(!load(std::move(source),absolute))return false;
            settings_.addRecentFile(absolute);refreshRecentFiles();return true;}
        catch(const std::exception& error){wxui::showError(this,error);return false;}
    }
    bool openResource(neoshared::ResourceDocument source) override {
        try {if(!commitGeometry())return false;
            for(std::size_t i=0;i<documents_.size();++i)if(documents_[i].model&&documents_[i].source.identity==source.identity) {rememberView();showDocument(i);return true;}
            return load(std::move(source),{});}catch(const std::exception& error){wxui::showError(this,error);return false;}
    }
    bool canClose() override {
        if(!commitGeometry())return false;
        const auto previous=active_;
        for(std::size_t i=0;i<documents_.size();++i)if(documents_[i].model&&documents_[i].history.at(documents_[i].historyIndex).bytes!=documents_[i].saved) {rememberView();showDocument(i);if(!confirmActiveClose())return false;}
        if(active_!=previous) {rememberView();showDocument(previous);}return true;
    }
    void resourceContextChanged() override {
        try {
            Resolver host;std::map<std::tuple<std::filesystem::path,std::filesystem::path,std::filesystem::path>,Resolver> games;
            for(auto& d:documents_) {
                if(d.hostAssets) {if(!host)host=context_.captureResourceResolver();d.resolver=host;}
                else if(d.installation) {
                    auto [session,inserted]=games.try_emplace(std::make_tuple(d.installation->installPath,d.installation->overridePath,d.installation->tlkPath));
                    if(inserted) {session->second=captureGameResources(*d.installation);if(context_.captureModFolder)session->second=neoshared::bif::ModFolder::layer(context_.captureModFolder(),std::move(session->second));}
                    d.resolver=session->second;
                }
                if(d.hostAssets||d.installation) {d.assetInputs.clear();d.textures.clear();}
            }
            renderer_.clear();updatePreview();refresh();
        }catch(const std::exception& error){wxui::showError(this,error);}
    }
    void setAppearance(bool dark,double scale) override {darkMode_=dark;fontScale_=scale;applyAppearance();}
    std::vector<std::filesystem::path> openPaths() const override {
        std::set<std::filesystem::path> result;
        for(const auto& d:documents_) {result.insert(d.source.protectedInputs.begin(),d.source.protectedInputs.end());result.insert(d.assetInputs.begin(),d.assetInputs.end());if(!d.path.empty())result.insert(d.path);if(d.model&&d.model->tlk().loaded())result.insert(d.model->tlk().filename());}
        return {result.begin(),result.end()};
    }
private:
    std::size_t indexOf(const wxWindow* page) const {
        return std::size_t(std::find_if(documents_.begin(),documents_.end(),[&](const auto& d){return d.tabPage==page;})-documents_.begin());
    }
    void rememberView() {
        auto& d=document();d.game=game_->GetSelection();d.layout=layoutSource_->GetSelection();d.zoom=zoom_->GetSelection();d.grid=grid_->GetSelection();d.state=state_->GetSelection();d.preview=preview_->GetValue();d.allFields=all_->GetValue();d.bounds=bounds_->GetValue();d.scroll=canvas_->GetViewStart();
    }
    void showDocument(std::size_t index) {
        active_=index;auto& d=document();geometryDirty_=false;
        neotabs::changeSelectionToPage(tabs_,d.tabPage);
        game_->SetSelection(d.game);layoutSource_->SetSelection(d.layout);zoom_->SetSelection(d.zoom);grid_->SetSelection(d.grid);state_->SetSelection(d.state);preview_->SetValue(d.preview);all_->SetValue(d.allFields);bounds_->SetValue(d.bounds);
        renderer_.clear();renderer_.setK2(d.game==1);
        canvas_->setAssets(false);canvas_->setZoom(std::array<double,4>{0,.5,1,2}.at(d.zoom));canvas_->setGrid(std::array<int,4>{1,4,8,16}.at(d.grid));canvas_->setState(d.state);canvas_->setBounds(d.bounds);
        rebuildTree();showProperties();refresh();canvas_->Scroll(d.scroll);updatePreview();
    }
    bool confirmActiveClose() {
        if(!dirty())return true;
        const auto name=document().path.empty()?document().source.fileName:neoshared::pathToUtf8(document().path.filename());
        const int answer=wxMessageBox(wxui::toWx("Save changes to "+name+"?"),"Unsaved GUI layout",wxYES_NO|wxCANCEL|wxICON_QUESTION,this);
        return answer==wxNO||(answer==wxYES&&save(false));
    }
    bool closeDocument(std::size_t index) {
        if(!commitGeometry())return false;
        rememberView();auto* keep=document().tabPage;auto* closing=documents_.at(index).tabPage;
        const auto& target=documents_.at(index);
        if(target.model&&target.history.at(target.historyIndex).bytes!=target.saved) {showDocument(index);if(!confirmActiveClose())return false;}
        canvas_->setDocument(nullptr,-1);switching_=true;
        neotabs::deleteTabPage(tabs_,closing);documents_.erase(documents_.begin()+std::ptrdiff_t(index));
        if(documents_.empty()) {documents_.emplace_back();documents_.back().tabPage=neotabs::addTabPage(tabs_,"GUI layout",false);}
        switching_=false;showDocument(indexOf(keep==closing?neotabs::currentPage(tabs_):keep));return true;
    }
    void closeOtherDocuments() {
        const auto* keep=document().tabPage;
        for(std::size_t i=documents_.size();i>0;--i)if(documents_[i-1].tabPage!=keep&&!closeDocument(i-1))return;
        rememberView();showDocument(indexOf(keep));
    }
    void newGui() {
        if(!commitGeometry())return;
        Document d;d.model=std::make_unique<GffModel>();d.model->newFile("GUI");
        d.model->addField("","CONTROLTYPE","Int","2");d.model->addField("","ID","Int","0");d.model->addField("","TAG","CExoString","new_gui");d.model->addField("","ALPHA","Float","1");
        d.model->addField("","EXTENT","Struct");
        for(const auto& value:std::array<std::pair<const char*,const char*>,4>{{{"LEFT","0"},{"TOP","0"},{"WIDTH","640"},{"HEIGHT","480"}}})d.model->addField("EXTENT",value.first,"Int",value.second);
        d.model->addField("","CONTROLS","List");
        d.source.identity="untitled:"+std::to_string(++nextUntitled_);d.source.fileName="Untitled"+std::to_string(nextUntitled_)+".gui";d.source.sourceDescription="New GUI layout";d.source.type=2047;
        addDocument(std::move(d),true);
    }
    Resolver gameResourcesFor(const neogames::GameInstall& installation) {
        const auto matches=[&](const Document& candidate) {
            return candidate.installation&&
                std::tie(candidate.installation->id,candidate.installation->installPath,candidate.installation->tlkPath,candidate.installation->overridePath)==
                std::tie(installation.id,installation.installPath,installation.tlkPath,installation.overridePath);
        };
        if(matches(document()))return document().resolver;
        for(const auto& candidate:documents_)if(matches(candidate))return candidate.resolver;
        auto result=captureGameResources(installation);
        if(context_.captureModFolder)result=neoshared::bif::ModFolder::layer(context_.captureModFolder(),std::move(result));
        return result;
    }
    void addDocument(Document d,bool modified=false) {
        d.history.push_back({d.model->gff().SaveBytes(),-1,{}});if(!modified)d.saved=d.history.front().bytes;
        d.resolver=document().resolver;d.installation=document().installation;d.hostAssets=document().hostAssets;d.assets=document().assets;d.game=game_->GetSelection();d.layout=1;
        const auto installation=context_.embedded&&d.path.empty()?std::optional<neogames::GameInstall>{}:guiInstallation(d.path,d.game);
        if(installation) {
            d.resolver=gameResourcesFor(*installation);d.installation=installation;d.hostAssets=false;
            d.game=installation->id=="kotor2"?1:0;
            d.assets=std::string(d.game==1?"KOTOR 2: ":"KOTOR 1: ")+neoshared::pathToUtf8(installation->installPath);
        }
        else if(context_.captureResourceResolver) {d.resolver=context_.captureResourceResolver();d.installation.reset();d.hostAssets=true;d.assets="NeoToolKit game browser";}
        else if(context_.resolveResource) {d.resolver=[resolve=context_.resolveResource](const std::string& name,std::uint16_t type,const neoshared::bif::JobControl&){return resolve(name,type);};d.installation.reset();d.assets="Game browser";}
        if(d.installation)d.model->loadTlk(d.installation->tlkPath);
        else if(document().model&&document().model->tlk().loaded())d.model->loadTlk(document().model->tlk().filename());
        d.preview=bool(d.resolver);
        d.layout=!d.path.empty()||d.source.priority<=neoshared::ResourcePriority::Directory?1:0;
        rememberView();
        if(document().model) {d.tabPage=neotabs::addTabPage(tabs_,d.source.fileName,modified,false);documents_.push_back(std::move(d));active_=documents_.size()-1;}
        else {d.tabPage=document().tabPage;document()=std::move(d);}
        showDocument(active_);
    }
    void applyAppearance() {
        if(!context_.embedded)darkModeItem_->Check(darkMode_);
        wxui::applyTheme(this,darkMode_);neoview::applyFontScale(this,fontScale_);canvas_->Refresh();
    }
    void changeFontScale(int steps) {
        fontScale_=neoview::steppedFontScale(fontScale_,steps);settings_.setFontScale(fontScale_);neoview::applyFontScale(this,fontScale_);
    }
    void refreshRecentFiles() {neosettings::populateRecentFilesMenu(*recentFilesMenu_,settings_,ID_RecentFirst,ID_ClearRecent);}
    void chooseOpen(const std::filesystem::path& directory={}) {
        wxFileDialog dialog(this,"Open GUI layouts",neosettings::pathToWx(directory),{},"GUI layouts (*.gui)|*.gui;*.GUI",wxFD_OPEN|wxFD_FILE_MUST_EXIST|wxFD_MULTIPLE);
        if(dialog.ShowModal()==wxID_OK) {wxArrayString paths;dialog.GetPaths(paths);for(const auto& path:paths)openFile(neosettings::pathFromWx(path));}
    }
    bool dirty() const {return document().model&&(geometryDirty_||document().history.at(document().historyIndex).bytes!=document().saved);}
    bool load(neoshared::ResourceDocument source,const std::filesystem::path& path) {
        Document candidate;candidate.model=std::make_unique<GffModel>();neoshared::loadGffResource(source,*candidate.model,"GUI");
        candidate.source=std::move(source);candidate.path=path;
        addDocument(std::move(candidate));return true;
    }
    void refreshCommands() {
        const bool loaded=bool(document().model),control=loaded&&document().selected>=0;
        for(int id:{ID_SaveAs,ID_Tlk,ID_Find,ID_FindNext,ID_Paste,ID_ImportControl,ID_GeneratePatcher})enableModuleCommand(id,loaded);
        enableModuleCommand(ID_Save,loaded&&(dirty()||document().path.empty()));
        for(int id:{ID_Copy,ID_Duplicate,ID_Delete})enableModuleCommand(id,control);
        enableModuleCommand(ID_Front,control&&std::size_t(document().selected+1)<controlList().count());enableModuleCommand(ID_Back,control&&document().selected>0);
        enableModuleCommand(ID_CopyValue,properties_->GetSelectedItemCount()!=0);
        const auto row=properties_->GetNextItem(-1,wxLIST_NEXT_ALL,wxLIST_STATE_SELECTED);
        enableModuleCommand(ID_EditValue,row>=0&&rows_.at(std::size_t(row)).editable);
        enableModuleCommand(ID_CloseOthers,documents_.size()>1);enableModuleCommand(ID_Next,documents_.size()>1);enableModuleCommand(ID_Previous,documents_.size()>1);
        enableModuleCommand(ID_HostAssets,bool(context_.captureResourceResolver));
        enableModuleCommand(ID_Undo,loaded&&(geometryDirty_||document().historyIndex>0));enableModuleCommand(ID_Redo,loaded&&!geometryDirty_&&document().historyIndex+1<document().history.size());
        for(int id:{ID_Save,ID_SaveAs,ID_Undo,ID_Redo,ID_ImportControl,ID_Duplicate,ID_Delete,ID_GeneratePatcher,ID_EditValue,ID_CopyValue})FindWindow(id)->Enable(commandEnabled(id));
        const auto saveLabel=document().path.empty()?"Save...":"Save";FindWindow(ID_Save)->SetLabel(saveLabel);saveItem_->SetItemLabel(wxString("&")+saveLabel+"\tCtrl+S");
        apply_->Enable(loaded&&geometryDirty_);revert_->Enable(loaded&&geometryDirty_);
        preview_->Enable(loaded&&bool(document().resolver));
        for(auto* choice:{game_,zoom_,grid_})choice->Enable(loaded);
        bounds_->Enable(loaded);layoutSource_->Enable(loaded&&game_->GetSelection()==0);state_->Enable(loaded&&preview_->GetValue());
    }
    void refresh() {
        if(document().model)renderer_.setLayout(document().path.empty()?document().source.fileName:neoshared::pathToUtf8(document().path.filename()),layoutSource_->GetSelection()==1);
        canvas_->setDocument(document().model?document().model->gff().root():nullptr,document().selected);
        setModuleTitle(wxui::toWx(std::string("NeoGUI — ")+(document().model?(document().path.empty()?document().source.fileName:neoshared::pathToUtf8(document().path.filename()))+(dirty()?" *":""):"GUI layout editor")));
        neotabs::setTabLabel(tabs_,document().tabPage,document().model?(document().path.empty()?document().source.fileName:neoshared::pathToUtf8(document().path.filename())):"GUI layout",dirty());
        const auto description=document().path.empty()?document().source.sourceDescription:document().source.sourceDescription+"\nWorking file: "+neoshared::pathToUtf8(document().path);
        tabs_->SetPageToolTip(std::size_t(neotabs::pageIndexFor(tabs_,document().tabPage)),wxui::toWx(description));
        auto assets=document().assets.empty()?std::string("Preview assets: choose a source from Assets"):"Preview assets: "+document().assets;
        if(document().model)assets+=document().model->tlk().loaded()?" | Talk table: "+neoshared::pathToUtf8(document().model->tlk().filename()):" | Talk table: not loaded (Assets menu)";
        assetInfo_->SetLabel(wxui::toWx(assets));assetInfo_->SetToolTip(wxui::toWx(assets));
        refreshCommands();
        setModuleStatusText(document().model?document().source.sourceDescription+" | "+std::to_string(controls(*document().model->gff().root()).count())+" controls | "+(dirty()?"Modified":"Saved"):"Open a GUI layout.");
    }
    void recordEdit(bool rebuild=true) {
        auto snapshot=document().model->gff().SaveBytes();
        if(snapshot!=document().history.at(document().historyIndex).bytes) {document().history.resize(document().historyIndex+1);document().history.push_back({std::move(snapshot),document().selected,document().fieldPath});++document().historyIndex;}
        if(rebuild)rebuildTree();
        showProperties();refresh();
    }
    void undo(std::size_t index) {const auto& snapshot=document().history.at(index);document().model->gff().LoadBytes(snapshot.bytes);document().model->updateTextFormat();document().selected=snapshot.selected;document().fieldPath=snapshot.fieldPath;document().historyIndex=index;rebuildTree();showProperties();refresh();updatePreview();}
    void rebuildTree() {
        rebuilding_=true;tree_->Freeze();tree_->DeleteAllItems();items_.clear();
        if(document().model) {
        const auto root=tree_->AddRoot(wxui::toWx(document().path.empty()?document().source.fileName:neoshared::pathToUtf8(document().path.filename())),-1,-1,new ControlData(-1));items_.push_back(root);
        for(std::size_t i=0;i<controls(*document().model->gff().root()).count();++i) {const auto& control=*controls(*document().model->gff().root()).GetStruct(i);const auto label=string(control,"TAG")+"  ["+std::to_string(integer(control,"ID",-1))+"]";items_.push_back(tree_->AppendItem(root,wxui::toWx(label),-1,-1,new ControlData(int(i))));}
        tree_->Expand(root);tree_->SelectItem(items_.at(std::size_t(document().selected+1)));}
        tree_->Thaw();rebuilding_=false;
    }
    std::string prefix() const {return document().selected<0?std::string{}:"CONTROLS\\"+std::to_string(document().selected)+"\\";}
    const GffStruct& selectedRecord() const {return document().selected<0?*document().model->gff().root():*controls(*document().model->gff().root()).GetStruct(std::size_t(document().selected));}
    void showProperties() {
        properties_->Freeze();properties_->DeleteAllItems();rows_.clear();geometryDirty_=false;document().allFields=all_->GetValue();all_->Enable(bool(document().model));for(auto* box:geometry_) {box->Enable(bool(document().model));box->ChangeValue({});}
        selectionInfo_->SetLabel("Open a layout, then select a control.");
        if(!document().model) {properties_->Thaw();refreshCommands();return;}
        const auto selection=document().selected<0?std::string("Layout bounds"):"Control bounds: "+string(selectedRecord(),"TAG")+" ["+std::to_string(integer(selectedRecord(),"ID"))+"]";
        selectionInfo_->SetLabel(wxui::toWx(selection));selectionInfo_->SetToolTip(wxui::toWx(selection));
        const auto rectangle=extent(selectedRecord());const std::array<double,4> dimensions={rectangle.m_x,rectangle.m_y,rectangle.m_width,rectangle.m_height};
        for(std::size_t i=0;i<geometry_.size();++i)geometry_[i]->ChangeValue(std::to_string(int(dimensions[i])));
        const auto selected=prefix();
        for(auto row:document().model->rows())if(all_->GetValue()||(selected.empty()?row.path.compare(0,9,"CONTROLS\\")!=0:row.path==selected.substr(0,selected.size()-1)||row.path.compare(0,selected.size(),selected)==0)) {
            const auto label=all_->GetValue()||selected.empty()?row.path:row.path==selected.substr(0,selected.size()-1)?"(Control)":row.path.substr(selected.size());
            const auto i=properties_->InsertItem(properties_->GetItemCount(),wxui::toWx(label));properties_->SetItem(i,1,wxui::toWx(row.type));properties_->SetItem(i,2,wxui::toWx(row.value));properties_->SetItem(i,3,wxui::toWx(row.resolved));rows_.push_back(std::move(row));
        }
        for(std::size_t i=0;i<rows_.size();++i)if(rows_[i].path==document().fieldPath) {properties_->SetItemState(long(i),wxLIST_STATE_SELECTED|wxLIST_STATE_FOCUSED,wxLIST_STATE_SELECTED|wxLIST_STATE_FOCUSED);properties_->EnsureVisible(long(i));break;}
        properties_->Thaw();refreshCommands();
    }
    void editProperty(std::size_t index) {
        if(!rows_.at(index).editable||!commitGeometry())return;
        const auto row=rows_.at(index);
        wxTextEntryDialog dialog(this,wxui::toWx(row.path+" ("+row.type+")"),"Edit GUI field",wxui::toWx(row.value),wxOK|wxCANCEL|wxRESIZE_BORDER|wxTE_MULTILINE);
        wxui::applyTheme(&dialog,darkMode_);neoview::applyFontScale(&dialog,fontScale_);
        dialog.SetSize(FromDIP(wxSize(550,240)));if(dialog.ShowModal()!=wxID_OK)return;
        try {document().model->setValue(row.path,wxui::toStd(dialog.GetValue()));recordEdit();updatePreview();}
        catch(const std::exception& error){wxui::showError(this,error);}
    }
    void editExtent(Rect r) {
        try {const auto base=prefix()+"EXTENT\\";const std::array<double,4> values={r.m_x,r.m_y,r.m_width,r.m_height};const char* labels[]={"LEFT","TOP","WIDTH","HEIGHT"};
            for(std::size_t i=0;i<values.size();++i)document().model->setValue(base+labels[i],std::to_string(int(std::lround(values[i]))));
            recordEdit(false);}
        catch(const std::exception& error){wxui::showError(this,error);}
    }
    bool applyExtent() {
        try {std::array<int,4> value;for(std::size_t i=0;i<value.size();++i)value[i]=neogff::ParseInt32Decimal(wxui::toStd(geometry_[i]->GetValue()));editExtent({double(value[0]),double(value[1]),double(value[2]),double(value[3])});return !geometryDirty_;}
        catch(const std::exception& error){wxui::showError(this,error);return false;}
    }
    bool commitGeometry() {return !geometryDirty_||applyExtent();}
    bool save(bool choose) {
        try {if(!commitGeometry())return false;auto output=document().path;
            const bool detached=choose||output.empty();
            if(detached) {wxFileDialog dialog(this,"Save GUI layout",neosettings::pathToWx(output.parent_path()),wxui::toWx(output.empty()?document().source.fileName:neoshared::pathToUtf8(output.filename())),"GUI layouts (*.gui)|*.gui;*.GUI",wxFD_SAVE|wxFD_OVERWRITE_PROMPT);if(dialog.ShowModal()!=wxID_OK)return false;output=neosettings::pathFromWx(dialog.GetPath());}
            neoshared::checkGffOutputType(output,".gui");auto inputs=document().source.protectedInputs;inputs.insert(inputs.end(),document().assetInputs.begin(),document().assetInputs.end());if(document().model->tlk().loaded())inputs.push_back(document().model->tlk().filename());
            for(const auto& other:documents_)if(&other!=&document()) {if(!other.path.empty())inputs.push_back(other.path);inputs.insert(inputs.end(),other.source.protectedInputs.begin(),other.source.protectedInputs.end());inputs.insert(inputs.end(),other.assetInputs.begin(),other.assetInputs.end());if(other.model&&other.model->tlk().loaded())inputs.push_back(other.model->tlk().filename());}
            neoshared::checkResourceOutput(output,inputs);validateHostOutput(output);document().model->gff().SaveFile(output);document().path=neosettings::storedPath(output);
            if(detached)layoutSource_->SetSelection(1);
            document().saved=document().history.at(document().historyIndex).bytes;settings_.addRecentFile(document().path);refreshRecentFiles();tree_->SetItemText(items_.front(),neosettings::pathToWx(document().path.filename()));refresh();return true;
        } catch(const std::exception& error){wxui::showError(this,error);return false;}
    }
    const neoshared::texture::TextureData& texture(const std::string& name) {
        const auto key=neoshared::material::textureResourceKey(name);if(const auto found=document().textures.find(key);found!=document().textures.end())return found->second;
        if(auto source=neoshared::resolveTexture(key,[this](const std::string& name,std::uint16_t type){return document().resolver(name,type,{});})) {
            std::string txi;if(source->type==3||source->type==2033)if(auto sidecar=document().resolver(key,2022,{})) {txi.assign(sidecar->bytes.begin(),sidecar->bytes.end());document().assetInputs.insert(sidecar->protectedInputs.begin(),sidecar->protectedInputs.end());}
            auto image=neoshared::texture::decodeTextureBytes(source->bytes,neoshared::pathFromUtf8(source->fileName),std::move(txi));document().assetInputs.insert(source->protectedInputs.begin(),source->protectedInputs.end());return document().textures.emplace(key,std::move(image)).first->second;
        }
        throw std::runtime_error("Missing GUI texture/font: "+name);
    }
    void cacheAssets() {renderer_.setLanguage(document().model->tlk().languageId());renderer_.cacheControl(*document().model->gff().root());for(const auto& control:controls(*document().model->gff().root()).allStructs())renderer_.cacheControl(*control);canvas_->Refresh();}
    void updatePreview() {
        try {if(preview_->GetValue())cacheAssets();canvas_->setAssets(preview_->GetValue());}
        catch(const std::exception& error){preview_->SetValue(false);canvas_->setAssets(false);wxui::showError(this,error);}
        document().preview=preview_->GetValue();
        refreshCommands();
    }
    void findNext(bool prompt) {
        if(!commitGeometry())return;
        if(prompt||document().findText.empty()) {
            wxTextEntryDialog dialog(this,"Search tags, IDs, field paths, values and resolved text:","Find GUI control or field",wxui::toWx(document().findText));
            wxui::applyTheme(&dialog,darkMode_);neoview::applyFontScale(&dialog,fontScale_);
            if(dialog.ShowModal()!=wxID_OK)return;
            document().findText=wxui::toStd(dialog.GetValue());document().findPath.clear();if(document().findText.empty())return;
        }
        const auto fields=document().model->rows();const auto query=wxui::toWx(document().findText).Lower();
        const auto previous=std::find_if(fields.begin(),fields.end(),[&](const auto& row){return row.path==document().findPath;});
        const auto start=previous==fields.end()?0:std::size_t(previous-fields.begin()+1);
        for(std::size_t offset=0;offset<fields.size();++offset) {
            const auto& row=fields[(start+offset)%fields.size()];
            if(!wxui::toWx(row.path+" "+row.value+" "+row.resolved).Lower().Contains(query))continue;
            document().findPath=row.path;
            const auto selected=row.path.compare(0,9,"CONTROLS\\")==0?int(std::stoul(row.path.substr(9))):-1;
            tree_->SelectItem(items_.at(std::size_t(selected+1)));document().fieldPath=row.path;showProperties();properties_->SetFocus();
            setModuleStatusText("Found: "+row.path);return;
        }
        setModuleStatusText("No match for: "+document().findText);
    }
    GffList& controlList() {return *static_cast<GffList*>(document().model->gff().root()->GetFieldByLabel("CONTROLS"));}
    void appendControl(const GffStruct& original,int offset=0) {
        auto copy=std::unique_ptr<GffStruct>(static_cast<GffStruct*>(original.Clone().release()));
        std::set<int> ids;std::set<std::string> tags;
        ids.insert(integer(*document().model->gff().root(),"ID"));tags.insert(string(*document().model->gff().root(),"TAG"));
        for(const auto& control:controlList().allStructs()) {ids.insert(integer(*control,"ID"));tags.insert(string(*control,"TAG"));}
        int id=1;while(ids.count(id))++id;
        auto tag=string(*copy,"TAG");const auto base=tag;int suffix=1;while(tags.count(tag))tag=base+"_"+std::to_string(suffix++);
        static_cast<GffIntField*>(copy->GetFieldByLabel("ID"))->value=id;
        static_cast<GffExoStringField*>(copy->GetFieldByLabel("TAG"))->SetString(tag);
        auto* bounds=static_cast<GffStruct*>(copy->GetFieldByLabel("EXTENT"));
        static_cast<GffIntField*>(bounds->GetFieldByLabel("LEFT"))->value+=offset;static_cast<GffIntField*>(bounds->GetFieldByLabel("TOP"))->value+=offset;
        controlList().AddStruct(std::move(copy));document().selected=int(controlList().count()-1);document().fieldPath=prefix()+"TAG";recordEdit();updatePreview();
    }
    void copyControl() {
        GffFile clipboard;clipboard.NewFile("GUI ");auto list=std::make_unique<GffList>("CONTROLS");
        list->AddStruct(std::unique_ptr<GffStruct>(static_cast<GffStruct*>(selectedRecord().Clone().release())));clipboard.root()->AddField(std::move(list));const auto bytes=clipboard.SaveBytes();
        wxClipboardLocker lock;if(!lock)throw std::runtime_error("Cannot open the clipboard.");
        auto* data=new wxCustomDataObject(wxDataFormat("NeoGUI.Control"));data->SetData(bytes.size(),bytes.data());
        if(!wxTheClipboard->SetData(data))throw std::runtime_error("Cannot copy the GUI control.");
        wxTheClipboard->Flush();
    }
    void pasteControl() {
        wxCustomDataObject data(wxDataFormat("NeoGUI.Control"));
        {wxClipboardLocker lock;if(!lock||!wxTheClipboard->GetData(data))throw std::runtime_error("Copy a GUI control before pasting.");}
        const auto* bytes=static_cast<const std::uint8_t*>(data.GetData());GffFile clipboard;clipboard.LoadBytes(std::vector<std::uint8_t>(bytes,bytes+data.GetSize()));appendControl(*controls(*clipboard.root()).GetStruct(0));
    }
    void importControl() {
        wxFileDialog dialog(this,"Add a control from another GUI layout",{},{},"GUI layouts (*.gui)|*.gui;*.GUI",wxFD_OPEN|wxFD_FILE_MUST_EXIST);if(dialog.ShowModal()!=wxID_OK)return;
        const auto path=neosettings::pathFromWx(dialog.GetPath());GffModel source;
        neoshared::loadGffResource({neoshared::pathToUtf8(path),neoshared::pathToUtf8(path.filename()),neoshared::pathToUtf8(path),2047,neoshared::texture::readFileBytes(path),{}},source,"GUI");
        const auto& list=controls(*source.gff().root());wxArrayString choices;
        for(const auto& control:list.allStructs())choices.Add(wxui::toWx(string(*control,"TAG")+" ["+std::to_string(integer(*control,"ID"))+"]"));
        if(choices.empty()) {setModuleStatusText("That layout contains no controls.");return;}
        wxSingleChoiceDialog picker(this,"Choose the native control to add:","Add GUI control",choices);wxui::applyTheme(&picker,darkMode_);neoview::applyFontScale(&picker,fontScale_);
        if(picker.ShowModal()==wxID_OK)appendControl(*list.GetStruct(std::size_t(picker.GetSelection())));
    }
    void deleteControl() {const auto selected=document().selected;controlList().DeleteStruct(std::uint32_t(selected));document().selected=std::min(selected,int(controlList().count())-1);document().fieldPath.clear();recordEdit();updatePreview();}
    void reorderControl(bool front) {
        auto& controls=controlList().allStructs();auto moved=std::move(controls.at(std::size_t(document().selected)));controls.erase(controls.begin()+document().selected);
        if(front)controls.push_back(std::move(moved));else controls.insert(controls.begin(),std::move(moved));
        document().selected=front?int(controls.size()-1):0;document().fieldPath.clear();recordEdit();
    }
    void copyFieldValue() {
        const auto index=properties_->GetNextItem(-1,wxLIST_NEXT_ALL,wxLIST_STATE_SELECTED);wxClipboardLocker lock;
        if(!lock||!wxTheClipboard->SetData(new wxTextDataObject(wxui::toWx(rows_.at(std::size_t(index)).value))))throw std::runtime_error("Cannot copy the field value.");
        wxTheClipboard->Flush();
    }
    void generatePatcher() {
        if(!commitGeometry())return;
        const auto baseline=wxui::chooseOpenFile(this,"Choose clean/unmodified GUI baseline","GUI layouts (*.gui)|*.gui;*.GUI");
        if(!baseline)return;
        GffModel original;
        neoshared::ResourceDocument source{neoshared::pathToUtf8(*baseline),neoshared::pathToUtf8(baseline->filename()),neoshared::pathToUtf8(*baseline),2047,neoshared::texture::readFileBytes(*baseline),{}};
        neoshared::loadGffResource(source,original,"GUI");
        if(document().model->tlk().loaded())original.loadTlk(document().model->tlk().filename());
        const auto name=wxui::promptText(this,"Patch Target Filename","Installed GUI filename (usually the baseline's name):",neoshared::pathToUtf8(baseline->filename()));
        if(!name)return;
        neoshared::checkGffOutputType(neoshared::pathFromUtf8(*name),".gui");
        auto project=neotsl::diffGffFlatTable(original.toTable(),document().model->toTable(),*name,false);
        neotsl::throwIfUnsupported(project);
        const auto output=wxui::choosePatcherOutput(this);
        if(!output)return;
        if(!output->writesToIni()) {wxui::showIniFragmentDialog(this,"GUI Patcher INI Fragment",project,{*name});return;}
        project.assets.push_back({{},*name,std::move(source.bytes)});
        neoshared::checkResourceOutput(output->iniPath,openPaths());
        std::vector<std::filesystem::path> targets={output->iniPath,output->iniPath.parent_path()/"info.rtf"};
        for(const auto& asset:project.assets)targets.push_back(output->iniPath.parent_path()/asset.targetName);
        validateHostOutputs(targets);
        const auto report=neotsl::writePackageToIni(project,output->iniPath,true);
        wxui::showMessage(this,"GUI Patcher Package Generated",std::string(report.mergedExisting?"Merged GUI instructions into:\n":"Created installer INI:\n")+neoshared::pathToUtf8(report.iniPath)+"\n\nThe clean GUI baseline was staged beside the selected INI.");
    }
    void useGame(const neogames::GameInstall& installation) {
        if(!commitGeometry())return;
        wxBusyCursor busy;
        auto resolve=captureGameResources(installation);
        if(context_.captureModFolder)resolve=neoshared::bif::ModFolder::layer(context_.captureModFolder(),std::move(resolve));
        if(document().model)document().model->loadTlk(installation.tlkPath);
        document().resolver=std::move(resolve);document().installation=installation;document().hostAssets=false;
        document().assets=std::string(installation.id=="kotor2"?"KOTOR 2: ":"KOTOR 1: ")+neoshared::pathToUtf8(installation.installPath);
        game_->SetSelection(installation.id=="kotor2"?1:0);renderer_.setK2(game_->GetSelection()==1);
        resetAssets();
    }
    void assetFolder(std::filesystem::path directory={}) {
        if(directory.empty()) {wxDirDialog dialog(this,"Choose extracted GUI textures and fonts");if(dialog.ShowModal()!=wxID_OK)return;directory=neosettings::pathFromWx(dialog.GetPath());}
        if(!commitGeometry())return;
        auto files=std::make_shared<std::map<Key,std::filesystem::path>>();
        for(const auto& entry:std::filesystem::directory_iterator(directory)) {
            auto extension=neoshared::pathToUtf8(entry.path().extension());std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return char(std::tolower(c));});
            for(const auto& [type,suffix]:textureTypes)if(extension==suffix)(*files)[{neoshared::material::textureResourceKey(neoshared::pathToUtf8(entry.path().stem())),type}]=entry.path();
            if(extension==".txi")(*files)[{neoshared::material::textureResourceKey(neoshared::pathToUtf8(entry.path().stem())),2022}]=entry.path();
        }
        document().installation.reset();document().hostAssets=false;document().assets="Extracted folder: "+neoshared::pathToUtf8(directory);document().resolver=[files](const std::string& name,std::uint16_t type,const neoshared::bif::JobControl&)->std::optional<neoshared::ResourceDocument> {
            const auto file=files->find({name,type});if(file==files->end())return {};
            const auto& path=file->second;return neoshared::ResourceDocument{neoshared::pathToUtf8(path),neoshared::pathToUtf8(path.filename()),neoshared::pathToUtf8(path),type,neoshared::texture::readFileBytes(path),{path}};
        };resetAssets();
    }
    void resetAssets() {canvas_->setAssets(false);document().textures.clear();document().assetInputs.clear();renderer_.clear();showProperties();refresh();updatePreview();}
    void command(int id) {
        try {switch(id) {
            case ID_New:newGui();break;
            case ID_Open:chooseOpen();break;
            case ID_Save:save(false);break;case ID_SaveAs:save(true);break;
            case ID_Close:closeDocument(active_);break;case ID_CloseOthers:closeOtherDocuments();break;
            case ID_Next:tabs_->AdvanceSelection(true);break;case ID_Previous:tabs_->AdvanceSelection(false);break;
            case ID_Find:findNext(true);break;case ID_FindNext:findNext(false);break;
            case ID_Copy:if(commitGeometry())copyControl();break;
            case ID_Paste:if(commitGeometry())pasteControl();break;
            case ID_Duplicate:if(commitGeometry())appendControl(selectedRecord(),std::max(8,std::array<int,4>{1,4,8,16}.at(grid_->GetSelection())));break;
            case ID_Delete:if(commitGeometry())deleteControl();break;
            case ID_ImportControl:if(commitGeometry())importControl();break;
            case ID_Front:if(commitGeometry())reorderControl(true);break;case ID_Back:if(commitGeometry())reorderControl(false);break;
            case ID_CopyValue:copyFieldValue();break;
            case ID_EditValue:editProperty(std::size_t(properties_->GetNextItem(-1,wxLIST_NEXT_ALL,wxLIST_STATE_SELECTED)));break;
            case ID_RevertGeometry:showProperties();refresh();break;
            case ID_GeneratePatcher:generatePatcher();break;
            case ID_Undo:if(geometryDirty_) {showProperties();refresh();}else undo(document().historyIndex-1);break;case ID_Redo:undo(document().historyIndex+1);break;
            case ID_Assets:assetFolder();break;
            case ID_HostAssets: if(commitGeometry()) {auto resolve=context_.captureResourceResolver();document().installation.reset();document().hostAssets=true;document().assets="NeoToolKit game browser";document().resolver=std::move(resolve);resetAssets();}break;
            case ID_Tlk: {wxFileDialog dialog(this,"Load GUI talk table",{},{},"Talk tables (*.tlk)|*.tlk;*.TLK",wxFD_OPEN|wxFD_FILE_MUST_EXIST);if(dialog.ShowModal()==wxID_OK&&commitGeometry()) {document().model->loadTlk(neosettings::pathFromWx(dialog.GetPath()));showProperties();refresh();updatePreview();}break;}
            case ID_DarkMode:darkMode_=darkModeItem_->IsChecked();settings_.setDarkMode(darkMode_);applyAppearance();break;
            case ID_FontIncrease:fontScaleWheelFilter_.reset();changeFontScale(1);break;
            case ID_FontDecrease:fontScaleWheelFilter_.reset();changeFontScale(-1);break;
            case ID_FontReset:fontScaleWheelFilter_.reset();fontScale_=neoview::kDefaultFontScale;settings_.setFontScale(fontScale_);neoview::applyFontScale(this,fontScale_);break;
            case ID_ClearRecent:settings_.clearRecentFiles();refreshRecentFiles();break;
            case ID_Exit:requestModuleClose();break;
            case ID_About:wxMessageBox("NeoGUI\nEdit GUI layouts and preview game textures, fonts and control states.","About NeoGUI",wxOK|wxICON_INFORMATION,this);break;
        }}catch(const std::exception& error){wxui::showError(this,error);}
    }
    Document& document() {return documents_.at(active_);}
    const Document& document() const {return documents_.at(active_);}
    std::vector<Document> documents_{1};
    std::size_t active_{};
    unsigned nextUntitled_{};
    wxAuiNotebook* tabs_{};
    bool switching_{},geometryDirty_{};
    neosettings::AppSettings settings_{"NeoGUI"};
    neoview::FontScaleWheelFilter fontScaleWheelFilter_;
    std::unique_ptr<neogames::OpenGameDirectoryMenu> gameDirectoryMenu_;
    wxMenu* recentFilesMenu_{};
    wxMenuItem* darkModeItem_{};
    wxMenuItem* saveItem_{};
    wxStaticText* assetInfo_{};wxStaticText* selectionInfo_{};
    bool darkMode_{};
    double fontScale_{neoview::kDefaultFontScale};
    GuiRenderer renderer_;
    Canvas* canvas_{};
    wxTreeCtrl* tree_{};
    wxListCtrl* properties_{};
    wxChoice* game_{},*layoutSource_{},*zoom_{},*grid_{},*state_{};
    wxCheckBox* preview_{};wxCheckBox* all_{};wxCheckBox* bounds_{};
    wxButton* apply_{};wxButton* revert_{};
    std::array<wxTextCtrl*,4> geometry_{};
    std::vector<wxTreeItemId> items_;
    std::vector<GffFieldRow> rows_;
    bool rebuilding_{};
};
}
GUIEditorPanel* createEditorPanel(wxWindow* parent,neomodules::Context context) {return new Editor(parent,std::move(context));}
}
