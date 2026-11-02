#include "GuiRenderer.hpp"
#include <neoshared/texture/Txi.hpp>
#include <neotlk/TextEncoding.hpp>
#include <wx/image.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <sstream>

namespace neogui {
void GuiRenderer::setLayout(std::string name,bool directory) {
    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::tolower(c));});
    const auto extension=name.rfind('.');if(extension!=std::string::npos)name.resize(extension);
    double width=640;
    for(const auto& size:std::array<std::pair<const char*,double>,6>{{{"16x12",1600},{"12x10",1280},{"12x9",1280},{"10x8",1024},{"10x7",1024},{"8x6",800}}}) {
        const std::string suffix=size.first;
        if(name.size()>=suffix.size()&&name.compare(name.size()-suffix.size(),suffix.size(),suffix)==0) {width=size.second;break;}
    }
    layoutScale_=directory||name.rfind("pazaak",0)==0?1:1024/width;
}
Rect GuiRenderer::layoutExtent(Rect rect) const {
    const auto scale=layoutScale();
    if(scale==1)return rect;
    const auto right=int((rect.m_x+rect.m_width)*scale+.5),bottom=int((rect.m_y+rect.m_height)*scale+.5);
    rect.m_x=int(rect.m_x*scale+.5);rect.m_y=int(rect.m_y*scale+.5);
    rect.m_width=right-rect.m_x;rect.m_height=bottom-rect.m_y;return rect;
}
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
    if(!k2_&&string(record,"CONTROLTYPE")=="11")dimension=int(dimension*layoutScale()+.5);
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
    if(!k2_&&string(record,"CONTROLTYPE")=="11")dimension=int(dimension*layoutScale()+.5);
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
void GuiRenderer::image(wxGraphicsContext& dc,const GffStruct& params,Rect area,int flip) {
    const auto* rotate=field<GffFloatField>(params,"ROTATE");
    const auto legacy=rotate?0:integer(params,"ROTATESTYLE");
    const auto turn=rotate?int(rotate->value/90):(legacy>=0&&legacy<4?legacy:0);
    const auto name=string(params,"IMAGE");
    if(!name.empty()&&integer(params,"DRAWSTYLE")) {
        const auto& pixels=texture(name).layers.front();const double width=std::min(area.m_width,double(pixels.width)),height=std::min(area.m_height,double(pixels.height));const auto alignment=integer(params,"ALIGNMENT",18);
        if((alignment&7)==2)area.m_x+=int((area.m_width-width)/2);else if((alignment&7)==4)area.m_x+=area.m_width-width;
        if((alignment&56)==16)area.m_y+=int((area.m_height-height)/2);else if((alignment&56)==32)area.m_y+=area.m_height-height;
        area.m_width=width;area.m_height=height;
    }
    stamp(dc,name,area,{1,1,1},1,(-turn%4+4)%4,(flip&12)==8,(flip&12)==4);
}
void GuiRenderer::slider(wxGraphicsContext& dc,const GffStruct& style,Rect rect,bool highlight) {
    border(dc,style,highlight?"HILIGHT":"BORDER",rect);
    if(const auto* params=field<GffStruct>(style,"THUMB")) {
        const auto name=string(*params,"IMAGE");
        if(!name.empty()) {
            const auto& pixels=texture(name).layers.front();Rect thumb(0,0,int(pixels.width*itemScale()),int(pixels.height*itemScale()));
            const auto maximum=std::max(0,integer(style,"MAXVALUE")),current=std::clamp(integer(style,"CURVALUE"),0,maximum);
            const bool vertical=rect.m_height>rect.m_width;
            if(vertical) {
                thumb.m_width=rect.m_width;thumb.m_x=rect.m_x;
                if(maximum>0) {if(rect.m_height>thumb.m_height)thumb.m_y=rect.m_y+current*int(rect.m_height-thumb.m_height)/maximum;else thumb.m_height=rect.m_height;}
                else {thumb.m_height=std::min(thumb.m_height,rect.m_height);thumb.m_y=rect.m_y;}
            } else {
                thumb.m_height=rect.m_height;thumb.m_y=rect.m_y;
                if(maximum>0) {if(rect.m_width>thumb.m_width)thumb.m_x=rect.m_x+current*int(rect.m_width-thumb.m_width)/maximum;else thumb.m_width=rect.m_width;}
                else {thumb.m_width=std::min(thumb.m_width,rect.m_width);thumb.m_x=rect.m_x;}
            }
            image(dc,*params,thumb,integer(*params,"FLIPSTYLE"));
        }
    }
}
void GuiRenderer::scrollbar(wxGraphicsContext& dc,const GffStruct& style,Rect rect,int maximum,int current,int visible) {
    maximum=std::max(0,maximum);current=std::clamp(current,0,maximum);if(visible<0)visible=1;
    const bool vertical=rect.m_height>rect.m_width,drawMode=byte(style,"DRAWMODE")!=0;
    const double arrow=std::min(vertical?rect.m_width:rect.m_height,std::floor((vertical?rect.m_height:rect.m_width)/2));
    const Rect track=vertical?Rect(rect.m_x,rect.m_y+arrow,rect.m_width,rect.m_height-2*arrow):Rect(rect.m_x+arrow,rect.m_y,rect.m_width-2*arrow,rect.m_height);
    if(!drawMode) {
        auto thumb=border(dc,style,"BORDER",track);
        if(vertical?maximum>=2:maximum!=0) {
            const auto length=int(vertical?thumb.m_height:thumb.m_width);
            const auto size=std::max(4,int(float(visible)/float(visible+maximum)*float(length)));
            const auto offset=(length-size)*current/(vertical?maximum-1:maximum);
            if(vertical) {thumb.m_y+=offset;thumb.m_height=size;}else {thumb.m_x+=offset;thumb.m_width=size;}
        }
        const auto& params=*field<GffStruct>(style,"THUMB");image(dc,params,thumb,integer(params,"FLIPSTYLE"));
    }
    const auto limit=drawMode&&visible<=1&&current!=maximum?maximum+1:maximum;
    if(!drawMode||(visible<maximum&&current>0))image(dc,*field<GffStruct>(style,"DIR"),vertical?Rect(rect.m_x,rect.m_y,rect.m_width,arrow):Rect(rect.m_x,rect.m_y,arrow,rect.m_height),0);
    if(!drawMode||(visible<maximum&&current+visible<limit))image(dc,*field<GffStruct>(style,"DIR"),vertical?Rect(rect.m_x,rect.GetBottom()-arrow,rect.m_width,arrow):Rect(rect.GetRight()-arrow,rect.m_y,arrow,rect.m_height),vertical?4:8);
}
double GuiRenderer::text(wxGraphicsContext& dc,const GffStruct& style,const std::string& value,Rect rect,double scale,std::optional<Color> overrideTint,double scroll) {
    if(value.empty())return 0;
    if(!k2_&&rect.m_width<30)scale=1;
    auto name=string(style,"FONT");if(name.empty())name="dialogfont16x16";auto& f=font(name);const auto& pixels=texture(name).layers.front();
    const auto bytes=neotlk::encodeTextBytes(value,neotlk::detectClassicPreferredEncoding(language_,false,{}));
    const auto glyphWidth=[&](unsigned char c){const auto i=std::min<std::size_t>(c,f.upper.size()-1);return (f.lower.at(i).m_x-f.upper.at(i).m_x)*f.width*scale;};
    const auto advance=[&](unsigned char c){return glyphWidth(c)+f.spacing*scale;};
    const auto rounded=[&](double width){return k2_?int(width):int(std::ceil(width));};
    std::vector<std::pair<std::string,int>> lines;
    const auto maximum=std::int16_t(rect.m_width);const auto oWidth=rounded(advance('o'));
    if((bytes.size()==1&&maximum<oWidth)||(bytes.size()<=2&&maximum<2*oWidth))return 0;
    for(std::size_t begin=0;begin<bytes.size()&&lines.size()<(k2_?100u:1001u);) {
        if(bytes[begin]=='\n') {lines.emplace_back("",0);++begin;continue;}
        int width=rounded(glyphWidth(static_cast<unsigned char>(bytes[begin]))),breakWidth=0;
        auto scan=begin;auto split=std::string::npos;
        if(width<maximum)for(scan=begin+1;scan<=bytes.size();++scan) {
            const auto previous=static_cast<unsigned char>(bytes[scan-1]);
            if((k2_&&previous==' ')||(scan>1&&scan<bytes.size()&&previous=='-'&&std::isalpha(static_cast<unsigned char>(bytes[scan-2]))&&std::isalpha(static_cast<unsigned char>(bytes[scan])))) {split=scan-1;breakWidth=width;}
            if(scan==bytes.size()||bytes[scan]=='\n')break;
            width+=k2_?int(advance(static_cast<unsigned char>(bytes[scan]))+.25):int(std::ceil(advance(static_cast<unsigned char>(bytes[scan]))+.25));
            if(bytes[scan]==' ') {split=scan;breakWidth=width;}
            if(width>=maximum)break;
        }
        if(scan==bytes.size()||bytes[scan]=='\n') {lines.emplace_back(bytes.substr(begin,scan-begin),width);begin=scan+(scan<bytes.size());}
        else if(split!=std::string::npos) {
            const bool space=bytes[split]==' ';lines.emplace_back(bytes.substr(begin,split-begin+(space?0:1)),breakWidth-(space?rounded(advance(' ')):0));begin=split+1;
        } else if(scan>begin+1) {
            const auto last=scan-1;lines.emplace_back(bytes.substr(begin,last-begin)+"-",std::uint16_t(width-rounded(advance(static_cast<unsigned char>(bytes[scan])))-rounded(advance(static_cast<unsigned char>(bytes[last]))))+rounded(advance('-')));begin=last;
        } else return 0;
    }
    const auto tint=overrideTint.value_or(color(style));const int alignment=integer(style,"ALIGNMENT",9);const double height=f.height*scale;
    const double blockHeight=(f.height+f.lineSpacing)*scale*lines.size();
    const double ideal=k2_?int(blockHeight+.5)+(int(f.height*scale+.5)<16):std::ceil(blockHeight+.5)+1;
    double y=rect.m_y;std::size_t first=0;
    if((alignment&56)==16) {auto offset=(rect.m_height-height*lines.size())/2;while(offset<0&&first<lines.size()) {offset+=height;++first;}y+=offset;}
    else if((alignment&56)==32) {auto block=height*lines.size();while(block>rect.m_height&&first<lines.size()) {block-=height;++first;}y+=rect.m_height-block;}
    y-=std::clamp(scroll,0.0,std::max(0.0,ideal-rect.m_height));
    dc.PushState();dc.Clip(rect.m_x,rect.m_y,rect.m_width,rect.m_height);
    for(std::size_t row=first;row<lines.size();++row) {
        const auto& line=lines[row].first;
        double w=k2_?lines[row].second:0;if(!k2_)for(unsigned char c:line)w+=advance(c);
        double x=rect.m_x;if((alignment&7)==2)x+=(rect.m_width-w)/2;else if((alignment&7)==4)x+=rect.m_width-w;
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
        const auto next=row+1<lines.size()&&!lines[row+1].first.empty()?static_cast<unsigned char>(lines[row+1].first.front()):0;
        const auto i=std::min<std::size_t>(next,f.upper.size()-1);const auto glyphHeight=(f.upper[i].m_y-f.lower[i].m_y)*f.width*scale;
        y+=glyphHeight+(k2_?f.width*scale/600:1)*f.lineSpacing*scale;
    }
    dc.PopState();return ideal;
}
}
