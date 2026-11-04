#pragma once
#include "GuiData.hpp"
#include <neoshared/texture/Image.hpp>
#include <wx/bitmap.h>
#include <wx/graphics.h>
#include <functional>
#include <map>
#include <optional>

namespace neogui {
class GuiRenderer {
public:
    explicit GuiRenderer(std::function<const neoshared::texture::TextureData&(const std::string&)> texture)
        :texture_(std::move(texture)) {}
    void setK2(bool value) {k2_=value;}
    void setLanguage(std::uint32_t value) {language_=value;}
    void setViewport(Rect value) {viewport_=value;}
    void setLayout(std::string name,bool directory);
    double layoutScale() const {return k2_?1:layoutScale_;}
    Rect layoutExtent(Rect) const;
    double fontScale() const {return k2_?1:1.3;}
    double itemScale() const {return k2_?1:1.6;}
    int padding(const GffStruct& record) const {return int(integer(record,"PADDING")*layoutScale()+.5);}
    void clear() {fonts_.clear();bitmaps_.clear();}
    void cacheControl(const GffStruct& control);
    void cacheFont(const std::string& name) {font(name);}
    void stamp(wxGraphicsContext&,const std::string&,const Rect&,const Color&,double alpha=1,int turn=0,bool flipX=false,bool flipY=false);
    Rect borderInner(const GffStruct&,const std::string&,Rect);
    Rect border(wxGraphicsContext&,const GffStruct&,const std::string&,Rect,std::optional<Color> tint={});
    void slider(wxGraphicsContext&,const GffStruct&,Rect,bool highlight);
    void scrollbar(wxGraphicsContext&,const GffStruct&,Rect,int maximum,int current,int visible);
    double text(wxGraphicsContext&,const GffStruct&,const std::string&,Rect,double scale,std::optional<Color> tint={},double scroll=0);
private:
    struct Font {double height{},width{},spacing{},lineSpacing{};std::vector<wxPoint2DDouble> upper,lower;};
    Font& font(const std::string&);
    void image(wxGraphicsContext&,const GffStruct&,Rect,int flip);
    const neoshared::texture::TextureData& texture(const std::string& name) {return texture_(name);}
    std::function<const neoshared::texture::TextureData&(const std::string&)> texture_;
    std::map<std::string,Font> fonts_;
    std::map<std::string,wxBitmap> bitmaps_;
    bool k2_{};
    double layoutScale_{1.6};
    Rect viewport_{0,0,1024,768};
    std::uint32_t language_{};
};
}
