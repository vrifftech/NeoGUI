#include "GuiRenderer.hpp"
#include <neoshared/texture/Txi.hpp>
#include <neotlk/TextEncoding.hpp>
#include <wx/image.h>
#include <algorithm>
#include <cmath>
#include <sstream>

namespace neogui {
GuiRenderer::Font& GuiRenderer::font(const std::string& name) {
    auto found=fonts_.find(name);if(found!=fonts_.end())return found->second;
    const auto& source=texture(name);Font value;std::vector<wxPoint2DDouble>* list=nullptr;std::size_t remaining=0;
    for(const auto& line:neoshared::texture::parseTxiEntries(source.txi)) {
        if(line.listData&&remaining) {std::istringstream row(line.value);double x,y,z;row>>x>>y>>z;list->push_back({x,y});--remaining;continue;}
        if(line.key=="fontheight")value.height=std::stod(line.value)*100;
        else if(line.key=="texturewidth")value.width=std::stod(line.value)*100;
        else if(line.key=="spacingr")value.spacing=std::stod(line.value)*100;
        else if(line.key=="spacingb")value.lineSpacing=std::stod(line.value)*100;
        else if(line.key=="upperleftcoords"||line.key=="lowerrightcoords") {list=line.key=="upperleftcoords"?&value.upper:&value.lower;remaining=std::stoul(line.value);}
    }
    return fonts_.emplace(name,std::move(value)).first->second;
}
void GuiRenderer::cacheControl(const GffStruct& control) {
    for(const auto* label:{"BORDER","HILIGHT","SELECTED","HILIGHTSELECTED","PROGRESS","SCROLLBAR","PROTOITEM","DIR","THUMB"})if(const auto* sub=field<GffStruct>(control,label))cacheControl(*sub);
    for(const auto* label:{"CORNER","EDGE","FILL","IMAGE","BACKGROUND"}) {const auto name=string(control,label);if(!name.empty())texture(name);}
    if(const auto* t=field<GffStruct>(control,"TEXT")) {const auto name=string(*t,"FONT");font(name.empty()?"dialogfont16x16":name);}
}
void GuiRenderer::stamp(wxGraphicsContext& dc,const std::string& name,const Rect& rect,const Color& tint,double alpha,int turn,bool flipX,bool flipY) {
    if(name.empty()||rect.m_width<=0||rect.m_height<=0)return;
    const auto key=name+"/"+std::to_string(tint[0])+"/"+std::to_string(tint[1])+"/"+std::to_string(tint[2])+"/"+std::to_string(alpha)+"/"+std::to_string(turn)+std::to_string(flipX)+std::to_string(flipY);
    auto bitmap=bitmaps_.find(key);
    if(bitmap==bitmaps_.end()) {
        const auto& pixels=texture(name).layers.front();wxImage image(int(pixels.width),int(pixels.height));image.InitAlpha();
        for(std::size_t i=0;i<pixels.rgba.size()/4;++i) {image.GetData()[3*i]=std::uint8_t(pixels.rgba[4*i]*tint[0]);image.GetData()[3*i+1]=std::uint8_t(pixels.rgba[4*i+1]*tint[1]);image.GetData()[3*i+2]=std::uint8_t(pixels.rgba[4*i+2]*tint[2]);image.GetAlpha()[i]=std::uint8_t(pixels.rgba[4*i+3]*alpha);}
        if(flipX)image=image.Mirror();
        if(flipY)image=image.Mirror(false);
        for(int i=0;i<turn;++i)image=image.Rotate90();
        bitmap=bitmaps_.emplace(key,wxBitmap(image)).first;
    }
    dc.DrawBitmap(bitmap->second,rect.m_x,rect.m_y,rect.m_width,rect.m_height);
}
Rect GuiRenderer::borderInner(const GffStruct& record,const std::string& label,Rect rect) {
    const auto* style=field<GffStruct>(record,label);if(!style)return rect;
    const auto corner=string(*style,"CORNER"),edge=string(*style,"EDGE");double dimension=integer(*style,"DIMENSION");
    if(dimension==0&&!corner.empty()&&!edge.empty())dimension=texture(corner).layers.front().height;
    Rect inner(rect.m_x+dimension,rect.m_y+dimension,rect.m_width-2*dimension,rect.m_height-2*dimension);
    if(inner.m_width<1||inner.m_width<dimension/2) {inner.m_x=rect.m_x+rect.m_width/2;inner.m_width=0;}
    if(inner.m_height<1||inner.m_height<dimension/2) {inner.m_y=rect.m_y+rect.m_height/2;inner.m_height=0;}
    const auto offset=integer(*style,"INNEROFFSET"),offsetY=!k2_?offset:integer(*style,"INNEROFFSETY",offset);
    const double padX=std::min(double(offset),dimension),padY=offsetY?offsetY:offset;
    return {inner.m_x-padX,inner.m_y-padY,inner.m_width+2*padX,inner.m_height+2*padY};
}
Rect GuiRenderer::border(wxGraphicsContext& dc,const GffStruct& record,const std::string& label,Rect rect,std::optional<Color> overrideTint) {
    const auto* style=field<GffStruct>(record,label);if(!style)return rect;
    const auto corner=string(*style,"CORNER"),edge=string(*style,"EDGE"),fill=string(*style,"FILL");const auto tint=overrideTint.value_or(color(*style));
    double dimension=integer(*style,"DIMENSION");
    if(dimension==0&&!corner.empty()&&!edge.empty())dimension=texture(corner).layers.front().height;
    double x=0,y=0;if(!corner.empty()) {const auto& p=texture(corner).layers.front();x=dimension?dimension:p.width;y=dimension?dimension:p.height;x=std::min(x,rect.m_width/2);y=std::min(y,rect.m_height/2);}
    const Rect inner(rect.m_x+x,rect.m_y+y,rect.m_width-2*x,rect.m_height-2*y);
    stamp(dc,corner,{rect.m_x,rect.m_y,x,y},tint);stamp(dc,corner,{rect.GetRight()-x,rect.m_y,x,y},tint,1,0,true);
    stamp(dc,corner,{rect.m_x,rect.GetBottom()-y,x,y},tint,1,0,false,true);stamp(dc,corner,{rect.GetRight()-x,rect.GetBottom()-y,x,y},tint,1,2);
    if(!edge.empty()) {
        const auto tile=dimension?dimension:double(texture(edge).layers.front().width);
        int columns=int(inner.m_width/tile),rows=int(inner.m_height/tile);
        if(columns==0&&inner.m_width>=tile/2)columns=1;
        if(rows==0&&inner.m_height>=tile/2)rows=1;
        for(int i=0;i<columns;++i) {const auto w=inner.m_width/columns;stamp(dc,edge,{inner.m_x+i*w,rect.m_y,w,y},tint);stamp(dc,edge,{inner.m_x+i*w,rect.GetBottom()-y,w,y},tint,1,2);}
        for(int i=0;i<rows;++i) {const auto h=inner.m_height/rows;stamp(dc,edge,{rect.m_x,inner.m_y+i*h,x,h},tint,1,1);stamp(dc,edge,{rect.GetRight()-x,inner.m_y+i*h,x,h},tint,1,3);}
    }
    if(!fill.empty()) {
        const auto styleId=integer(*style,"FILLSTYLE");
        if(styleId==2||(styleId==3&&k2_))stamp(dc,fill,styleId==3?rect:inner,tint);
        else if(styleId==0||styleId==1) {
            const auto& p=texture(fill).layers.front();dc.PushState();dc.Clip(inner.m_x,inner.m_y,inner.m_width,inner.m_height);
            if(styleId==1) {
                const auto width=std::min(inner.m_width,double(p.width)),height=std::min(inner.m_height,double(p.height));
                stamp(dc,fill,{inner.m_x+(inner.m_width-width)/2,inner.m_y+(inner.m_height-height)/2,width,height},tint);
            }
            else for(double yy=inner.m_y;yy<inner.GetBottom();yy+=p.height)for(double xx=inner.m_x;xx<inner.GetRight();xx+=p.width)stamp(dc,fill,{xx,yy,double(p.width),double(p.height)},tint);
            dc.PopState();
        }
    }
    return borderInner(record,label,rect);
}
void GuiRenderer::scrollbar(wxGraphicsContext& dc,const GffStruct& style,Rect rect,int maximum,int current,int visible) {
    maximum=std::max(0,maximum);current=std::clamp(current,0,maximum);if(visible<0)visible=1;
    const bool vertical=rect.m_height>rect.m_width,drawMode=byte(style,"DRAWMODE")!=0;
    const double arrow=std::min(vertical?rect.m_width:rect.m_height,std::floor((vertical?rect.m_height:rect.m_width)/2));
    const Rect track=vertical?Rect(rect.m_x,rect.m_y+arrow,rect.m_width,rect.m_height-2*arrow):Rect(rect.m_x+arrow,rect.m_y,rect.m_width-2*arrow,rect.m_height);
    const auto image=[&](const char* label,Rect area,int arrowFlip) {
        const auto& params=*field<GffStruct>(style,label);const auto turn=field<GffFloatField>(params,"ROTATE")?int(number(params,"ROTATE")/90):integer(params,"ROTATESTYLE");
        const auto name=string(params,"IMAGE");
        if(!name.empty()&&integer(params,"DRAWSTYLE")) {
            const auto& pixels=texture(name).layers.front();const double width=std::min(area.m_width,double(pixels.width)),height=std::min(area.m_height,double(pixels.height));const auto alignment=integer(params,"ALIGNMENT",18);
            if(alignment&2)area.m_x+=(area.m_width-width)/2;else if(alignment&4)area.m_x+=area.m_width-width;
            if(alignment&16)area.m_y+=(area.m_height-height)/2;else if(alignment&32)area.m_y+=area.m_height-height;
            area.m_width=width;area.m_height=height;
        }
        const int flip=std::string(label)=="DIR"?arrowFlip:integer(params,"FLIPSTYLE");
        stamp(dc,name,area,{1,1,1},1,(turn%4+4)%4,(flip&8)!=0,(flip&4)!=0);
    };
    if(!drawMode) {
        auto thumb=border(dc,style,"BORDER",track);
        if(vertical?maximum>=2:maximum!=0) {
            const auto length=int(vertical?thumb.m_height:thumb.m_width);
            const auto size=std::max(4,int(float(visible)/float(visible+maximum)*float(length)));
            const auto offset=(length-size)*current/(vertical?maximum-1:maximum);
            if(vertical) {thumb.m_y+=offset;thumb.m_height=size;}else {thumb.m_x+=offset;thumb.m_width=size;}
        }
        image("THUMB",thumb,0);
    }
    const auto limit=drawMode&&visible<=1&&current!=maximum?maximum+1:maximum;
    if(!drawMode||(visible<maximum&&current>0))image("DIR",vertical?Rect(rect.m_x,rect.m_y,rect.m_width,arrow):Rect(rect.m_x,rect.m_y,arrow,rect.m_height),0);
    if(!drawMode||(visible<maximum&&current+visible<limit))image("DIR",vertical?Rect(rect.m_x,rect.GetBottom()-arrow,rect.m_width,arrow):Rect(rect.GetRight()-arrow,rect.m_y,arrow,rect.m_height),vertical?4:8);
}
void GuiRenderer::text(wxGraphicsContext& dc,const GffStruct& style,const std::string& value,Rect rect,double scale,std::optional<Color> overrideTint) {
    if(value.empty())return;
    auto name=string(style,"FONT");if(name.empty())name="dialogfont16x16";auto& f=font(name);const auto& pixels=texture(name).layers.front();
    const auto bytes=neotlk::encodeTextBytes(value,neotlk::detectClassicPreferredEncoding(language_,false,{}));
    const auto advance=[&](unsigned char c){const auto i=std::min<std::size_t>(c,f.upper.size()-1);return ((f.lower.at(i).m_x-f.upper.at(i).m_x)*f.width+f.spacing)*scale;};
    std::vector<std::string> lines(1);double width=0;
    for(unsigned char c:bytes) {
        if(c=='\n') {lines.emplace_back();width=0;continue;}
        if(width+advance(c)>rect.m_width&&!lines.back().empty()) {
            const auto at=lines.back().find_last_of(" -");std::string tail;
            if(at!=std::string::npos) {const bool space=lines.back()[at]==' ';tail=lines.back().substr(at+1);lines.back().resize(at+(space?0:1));}
            lines.push_back(std::move(tail));width=0;for(unsigned char previous:lines.back())width+=advance(previous);
            if(c==' '&&lines.back().empty())continue;
        }
        lines.back().push_back(char(c));width+=advance(c);
    }
    const auto tint=overrideTint.value_or(color(style));const int alignment=integer(style,"ALIGNMENT",9);const double height=f.height*scale;
    double y=rect.m_y;if(alignment&16)y+=(rect.m_height-height*lines.size())/2;else if(alignment&32)y+=rect.m_height-height*lines.size();
    dc.PushState();dc.Clip(rect.m_x,rect.m_y,rect.m_width,rect.m_height);
    for(const auto& line:lines) {
        double w=0;for(unsigned char c:line)w+=advance(c);double x=rect.m_x;if(alignment&2)x+=(rect.m_width-w)/2;else if(alignment&4)x+=rect.m_width-w;
        for(unsigned char c:line) {
            const auto i=std::min<std::size_t>(c,f.upper.size()-1);const auto a=f.upper.at(i),b=f.lower.at(i);
            const auto key=name+" glyph "+std::to_string(i)+"/"+std::to_string(tint[0])+"/"+std::to_string(tint[1])+"/"+std::to_string(tint[2]);
            auto glyph=bitmaps_.find(key);const int xx=int(std::lround(a.m_x*pixels.width)),yy=int(std::lround((1-a.m_y)*pixels.height)),ww=int(std::lround((b.m_x-a.m_x)*pixels.width)),hh=int(std::lround((a.m_y-b.m_y)*pixels.height));
            if(ww>0&&hh>0) {
                if(glyph==bitmaps_.end()) {wxImage image(ww,hh);image.InitAlpha();for(int v=0;v<hh;++v)for(int u=0;u<ww;++u) {const auto src=std::size_t((yy+v)*pixels.width+xx+u)*4,dst=std::size_t(v*ww+u);image.GetData()[dst*3]=std::uint8_t(pixels.rgba.at(src)*tint[0]);image.GetData()[dst*3+1]=std::uint8_t(pixels.rgba.at(src+1)*tint[1]);image.GetData()[dst*3+2]=std::uint8_t(pixels.rgba.at(src+2)*tint[2]);image.GetAlpha()[dst]=pixels.rgba.at(src+3);}glyph=bitmaps_.emplace(key,wxBitmap(image)).first;}
                dc.DrawBitmap(glyph->second,x,y,(b.m_x-a.m_x)*f.width*scale,height);
            }
            x+=advance(c);
        }
        y+=height+f.lineSpacing*scale;
    }
    dc.PopState();
}
}
