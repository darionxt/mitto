#include "mitto/dom/trusted_type_policy_factory.h"

#include <algorithm>
#include <string_view>

#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/interfaces_without_class.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/v8_trusted_type_policy_factory.h"
#include "mitto/dom/html_names.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

namespace {

constexpr char16_t kXlinkNamespace[] = u"http://www.w3.org/1999/xlink";
constexpr char16_t kTrustedHTML[] = u"TrustedHTML";
constexpr char16_t kTrustedScript[] = u"TrustedScript";
constexpr char16_t kTrustedScriptURL[] = u"TrustedScriptURL";

// An entry of BuildAttributeVector()'s table: an element in a namespace, an
// attribute in a namespace or none, and the trusted type it takes.
struct AttributeType {
  std::u16string_view element;
  std::u16string_view element_namespace;
  std::u16string_view attribute;
  std::u16string_view attribute_namespace;
  const char16_t* type;
};

constexpr AttributeType kAttributeTypes[] = {
    {u"embed", html_names::kXhtmlNamespace, u"src", u"", kTrustedScriptURL},
    {u"iframe", html_names::kXhtmlNamespace, u"srcdoc", u"", kTrustedHTML},
    {u"object", html_names::kXhtmlNamespace, u"codebase", u"",
     kTrustedScriptURL},
    {u"object", html_names::kXhtmlNamespace, u"data", u"", kTrustedScriptURL},
    {u"script", html_names::kXhtmlNamespace, u"src", u"", kTrustedScriptURL},
    {u"script", html_names::kSvgNamespace, u"href", u"", kTrustedScriptURL},
    {u"script", html_names::kSvgNamespace, u"href", kXlinkNamespace,
     kTrustedScriptURL},
};

// Blink's EVENT_HANDLER_LIST of the Engine Release, which
// generate_eventhandler_names.py writes from the Web IDL database's event
// handler attributes, the event types of event_type_names.json5 with "on"
// before them, and the attributes beginning with "on" of the HTML, SVG and
// MathML attribute names, sorted. Each is the content attribute of an event
// handler, which takes a TrustedScript on any element.
constexpr std::u16string_view kEventHandlers[] = {
    u"onabort",
    u"onabortpayment",
    u"onactivate",
    u"onactive",
    u"onaddsourcebuffer",
    u"onaddstream",
    u"onaddtrack",
    u"onadvertisementreceived",
    u"onafterprint",
    u"onanimationcancel",
    u"onanimationend",
    u"onanimationiteration",
    u"onanimationstart",
    u"onappinstalled",
    u"onaudioend",
    u"onaudioprocess",
    u"onaudiostart",
    u"onautofill",
    u"onauxclick",
    u"onbackgroundfetchabort",
    u"onbackgroundfetchclick",
    u"onbackgroundfetchfail",
    u"onbackgroundfetchsuccess",
    u"onbeforecopy",
    u"onbeforecreatepolicy",
    u"onbeforecut",
    u"onbeforefilter",
    u"onbeforeinput",
    u"onbeforeinstallprompt",
    u"onbeforematch",
    u"onbeforepaste",
    u"onbeforeprint",
    u"onbeforetoggle",
    u"onbeforeunload",
    u"onbeforexrselect",
    u"onbegin",
    u"onbeginevent",
    u"onbitrateinfochange",
    u"onblocked",
    u"onblur",
    u"onboundary",
    u"onbufferedamountlow",
    u"oncancel",
    u"oncanmakepayment",
    u"oncanplay",
    u"oncanplaythrough",
    u"oncapturedmousechange",
    u"oncapturehandlechange",
    u"onchange",
    u"oncharacterboundsupdate",
    u"oncharacteristicvaluechanged",
    u"onchargingchange",
    u"onchargingtimechange",
    u"onchecked",
    u"onclick",
    u"onclipboardchange",
    u"onclose",
    u"onclosing",
    u"oncommand",
    u"oncomplete",
    u"oncompositionend",
    u"oncompositionstart",
    u"oncompositionupdate",
    u"onconfigurationchange",
    u"onconnect",
    u"onconnecting",
    u"onconnectionavailable",
    u"onconnectionstatechange",
    u"oncontentdelete",
    u"oncontentvisibilityautostatechange",
    u"oncontextlost",
    u"oncontextmenu",
    u"oncontextoverflow",
    u"oncontextrestored",
    u"oncontrollerchange",
    u"oncookiechange",
    u"oncopy",
    u"oncuechange",
    u"oncurrententrychange",
    u"oncurrentscreenchange",
    u"oncut",
    u"ondataavailable",
    u"ondatachannel",
    u"ondblclick",
    u"ondequeue",
    u"ondevicechange",
    u"ondevicemotion",
    u"ondeviceorientation",
    u"ondeviceorientationabsolute",
    u"ondischargingtimechange",
    u"ondisconnect",
    u"ondispose",
    u"ondomactivate",
    u"ondomcontentloaded",
    u"ondomfocusin",
    u"ondomfocusout",
    u"ondownloadprogress",
    u"ondrag",
    u"ondragend",
    u"ondragenter",
    u"ondragleave",
    u"ondragover",
    u"ondragstart",
    u"ondrop",
    u"ondurationchange",
    u"onelementgeometryupdate",
    u"onemptied",
    u"onencrypted",
    u"onend",
    u"onended",
    u"onendevent",
    u"onenter",
    u"onenterpictureinpicture",
    u"onerror",
    u"onexit",
    u"onfetch",
    u"onfinish",
    u"onfocus",
    u"onfocusin",
    u"onfocusout",
    u"onfocusreset",
    u"onformdata",
    u"onframeratechange",
    u"onfreeze",
    u"onfullscreenchange",
    u"onfullscreenerror",
    u"ongamepadconnected",
    u"ongamepaddisconnected",
    u"ongamepadrawinputchanged",
    u"ongatheringstatechange",
    u"ongattserverdisconnected",
    u"ongeometrychange",
    u"ongestureflingstart",
    u"ongesturelongpress",
    u"ongestureshowpress",
    u"ongesturetap",
    u"ongesturetapdown",
    u"ongesturetapunconfirmed",
    u"ongotpointercapture",
    u"onhashchange",
    u"onhdrheadroomchange",
    u"onicecandidate",
    u"onicecandidateerror",
    u"oniceconnectionstatechange",
    u"onicegatheringstatechange",
    u"oninactive",
    u"oninput",
    u"oninputreport",
    u"oninputsourceschange",
    u"oninstall",
    u"oninstallresult",
    u"oninterest",
    u"oninterfacerequest",
    u"oninvalid",
    u"onjobstatechange",
    u"onkeydown",
    u"onkeyframerequest",
    u"onkeypress",
    u"onkeystatuseschange",
    u"onkeyup",
    u"onlanguagechange",
    u"onleavepictureinpicture",
    u"onlevelchange",
    u"onload",
    u"onloadeddata",
    u"onloadedmetadata",
    u"onloadend",
    u"onloading",
    u"onloadingdone",
    u"onloadingerror",
    u"onloadstart",
    u"onlocation",
    u"onloseinterest",
    u"onlostpointercapture",
    u"onmanagedconfigurationchange",
    u"onmark",
    u"onmessage",
    u"onmessageerror",
    u"onmidimessage",
    u"onmousedown",
    u"onmouseenter",
    u"onmouseleave",
    u"onmousemove",
    u"onmouseout",
    u"onmouseover",
    u"onmouseup",
    u"onmousewheel",
    u"onmove",
    u"onmute",
    u"onnavigate",
    u"onnavigateerror",
    u"onnavigatesuccess",
    u"onnegotiationneeded",
    u"onnomatch",
    u"onnotificationclick",
    u"onnotificationclose",
    u"onoffline",
    u"ononline",
    u"onopen",
    u"onopenerchange",
    u"onorientationchange",
    u"onoverscrollcancel",
    u"onoverscrollchanging",
    u"onoverscrollend",
    u"onoverscrollstart",
    u"onpagehide",
    u"onpagereveal",
    u"onpageshow",
    u"onpageswap",
    u"onpaint",
    u"onpaste",
    u"onpause",
    u"onpayerdetailchange",
    u"onpaymentmethodchange",
    u"onpaymentrequest",
    u"onperiodicsync",
    u"onplay",
    u"onplaying",
    u"onpointercancel",
    u"onpointerdown",
    u"onpointerenter",
    u"onpointerleave",
    u"onpointerlockchange",
    u"onpointerlockerror",
    u"onpointermove",
    u"onpointerout",
    u"onpointerover",
    u"onpointerrawupdate",
    u"onpointerup",
    u"onpopoverhide",
    u"onpopovershow",
    u"onpopstate",
    u"onprerenderingchange",
    u"onprioritychange",
    u"onprocessorerror",
    u"onprogress",
    u"onpromptaction",
    u"onpromptdismiss",
    u"onpush",
    u"onpushsubscriptionchange",
    u"onquotaoverflow",
    u"onratechange",
    u"onreading",
    u"onreadingerror",
    u"onreadystatechange",
    u"onredraw",
    u"onreflectionchange",
    u"onrejectionhandled",
    u"onrelease",
    u"onremove",
    u"onremovesourcebuffer",
    u"onremovestream",
    u"onremovetrack",
    u"onrepeat",
    u"onrepeatevent",
    u"onreset",
    u"onresize",
    u"onresourcetimingbufferfull",
    u"onresult",
    u"onresume",
    u"onrtcsenderencodedsource",
    u"onrtctransform",
    u"onsamplebufferfull",
    u"onscreenschange",
    u"onscroll",
    u"onscrollend",
    u"onscrollsnapchange",
    u"onscrollsnapchanging",
    u"onsearch",
    u"onsecuritypolicyviolation",
    u"onseeked",
    u"onseeking",
    u"onselect",
    u"onselectedcandidatepairchange",
    u"onselectend",
    u"onselectionchange",
    u"onselectstart",
    u"onshippingaddresschange",
    u"onshippingoptionchange",
    u"onshow",
    u"onsignalingstatechange",
    u"onsinkchange",
    u"onslotchange",
    u"onsoundend",
    u"onsoundstart",
    u"onsourceclose",
    u"onsourceended",
    u"onsourceopen",
    u"onspeechend",
    u"onspeechstart",
    u"onsqueeze",
    u"onsqueezeend",
    u"onsqueezestart",
    u"onstalled",
    u"onstart",
    u"onstatechange",
    u"onstop",
    u"onstorage",
    u"onstream",
    u"onsubmit",
    u"onsuccess",
    u"onsuspend",
    u"onsync",
    u"onterminate",
    u"ontextformatupdate",
    u"ontextinput",
    u"ontextupdate",
    u"ontimeout",
    u"ontimeupdate",
    u"ontimezonechange",
    u"ontoggle",
    u"ontonechange",
    u"ontoolactivated",
    u"ontoolcancel",
    u"ontoolchange",
    u"ontouchcancel",
    u"ontouchend",
    u"ontouchmove",
    u"ontouchstart",
    u"ontrack",
    u"ontransitioncancel",
    u"ontransitionend",
    u"ontransitionrun",
    u"ontransitionstart",
    u"ontypechange",
    u"onuareplaceend",
    u"onuareplacestart",
    u"onunbounded",
    u"onuncapturederror",
    u"onunhandledrejection",
    u"onunload",
    u"onunmute",
    u"onupdate",
    u"onupdateend",
    u"onupdatefound",
    u"onupdatestart",
    u"onupgradeneeded",
    u"onvalidationstatuschange",
    u"onversionchange",
    u"onvisibilitychange",
    u"onvisibilitymaskchange",
    u"onvoiceschanged",
    u"onvolumechange",
    u"onwaiting",
    u"onwaitingforkey",
    u"onwebglcontextcreationerror",
    u"onwebglcontextlost",
    u"onwebglcontextrestored",
    u"onwebkitanimationend",
    u"onwebkitanimationiteration",
    u"onwebkitanimationstart",
    u"onwebkitbeforetextinserted",
    u"onwebkiteditablecontentchanged",
    u"onwebkitfullscreenchange",
    u"onwebkitfullscreenerror",
    u"onwebkitspeechchange",
    u"onwebkittransitionend",
    u"onwebkitvisibilitychange",
    u"onwheel",
    u"onwritablechange",
    u"onwrite",
    u"onwriteend",
    u"onwritestart",
    u"onzoomlevelchange",
};

bool IsInstance(v8::Local<v8::Value> value,
                const bindings::WrapperTypeInfo& info) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  return bindings::PerIsolateData::From(isolate)->HasInstance(isolate, info,
                                                              value);
}

}  // namespace

// static
TrustedTypePolicyFactory* TrustedTypePolicyFactory::Create() {
  return bindings::MakeGarbageCollected<TrustedTypePolicyFactory>();
}

// static
bindings::ScriptWrappable* TrustedTypePolicyFactory::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<TrustedTypePolicyFactory>(
      info, bindings::V8TrustedTypePolicyFactory::kWrapperTypeInfo);
}

TrustedTypePolicyFactory::TrustedTypePolicyFactory() = default;
TrustedTypePolicyFactory::~TrustedTypePolicyFactory() = default;

const bindings::WrapperTypeInfo* TrustedTypePolicyFactory::GetWrapperTypeInfo()
    const {
  return &bindings::V8TrustedTypePolicyFactory::kWrapperTypeInfo;
}

bool TrustedTypePolicyFactory::IsHTML(
    v8::Local<v8::Value> checked_object) const {
  return IsInstance(checked_object, bindings::V8TrustedHTML::kWrapperTypeInfo);
}

bool TrustedTypePolicyFactory::IsScript(
    v8::Local<v8::Value> checked_object) const {
  return IsInstance(checked_object,
                    bindings::V8TrustedScript::kWrapperTypeInfo);
}

bool TrustedTypePolicyFactory::IsScriptURL(
    v8::Local<v8::Value> checked_object) const {
  return IsInstance(checked_object,
                    bindings::V8TrustedScriptURL::kWrapperTypeInfo);
}

std::optional<std::u16string> TrustedTypePolicyFactory::GetAttributeType(
    const std::u16string& tag_name,
    const std::u16string& attribute,
    const std::optional<std::u16string>& element_ns,
    const std::optional<std::u16string>& attr_ns) const {
  // FindEntryInAttributeTypeVector(): names compare in lower case, an empty
  // or null element namespace is HTML's, and an empty or null attribute
  // namespace is none.
  const std::u16string element = base::ToLowerASCII(tag_name);
  const std::u16string name = base::ToLowerASCII(attribute);
  const std::u16string_view element_namespace =
      element_ns.value_or(u"").empty() ? html_names::kXhtmlNamespace
                                       : std::u16string_view(*element_ns);
  const std::u16string_view attribute_namespace =
      attr_ns ? std::u16string_view(*attr_ns) : std::u16string_view();
  for (const AttributeType& entry : kAttributeTypes) {
    if (entry.element == element &&
        entry.element_namespace == element_namespace &&
        entry.attribute == name &&
        entry.attribute_namespace == attribute_namespace) {
      return std::u16string(entry.type);
    }
  }
  // An event handler matches any element of the HTML, SVG and MathML
  // namespaces, with no attribute namespace.
  const bool any_element = element_namespace == html_names::kXhtmlNamespace ||
                           element_namespace == html_names::kSvgNamespace ||
                           element_namespace == html_names::kMathMlNamespace;
  if (any_element && attribute_namespace.empty() &&
      std::ranges::binary_search(kEventHandlers, std::u16string_view(name))) {
    return std::u16string(kTrustedScript);
  }
  return std::nullopt;
}

}  // namespace mitto
