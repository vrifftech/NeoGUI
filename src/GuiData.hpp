#pragma once
#include <gff/GFFFile.hpp>
#include <wx/geometry.h>
#include <array>

namespace neogui {
using Color=std::array<float,3>;
using Rect=wxRect2DDouble;
using namespace neogff;
template<class T> const T* field(const GffStruct& record,const std::string& name) {
    return dynamic_cast<const T*>(record.GetFieldByLabel(name));
}
inline int integer(const GffStruct& record,const std::string& name,int initial=0) {
    const auto* value=field<GffIntField>(record,name);return value?value->value:initial;
}
inline unsigned byte(const GffStruct& record,const std::string& name,unsigned initial=0) {
    const auto* value=field<GffByteField>(record,name);return value?value->value:initial;
}
inline double number(const GffStruct& record,const std::string& name,double initial=0) {
    const auto* value=field<GffFloatField>(record,name);return value?value->value:initial;
}
inline std::string string(const GffStruct& record,const std::string& name) {
    const auto* value=record.GetFieldByLabel(name);return value?value->GetString():std::string{};
}
inline Color color(const GffStruct& record) {
    const auto* value=field<GffPositionField>(record,"COLOR");return value?value->value:Color{1,1,1};
}
inline Rect extent(const GffStruct& record) {
    const auto& value=*field<GffStruct>(record,"EXTENT");
    return {double(integer(value,"LEFT")),double(integer(value,"TOP")),double(integer(value,"WIDTH")),double(integer(value,"HEIGHT"))};
}
inline const GffList& controls(const GffStruct& root) {return *field<GffList>(root,"CONTROLS");}
}
