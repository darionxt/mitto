// The pieces of a multipart/form-data body, as Blink's FormDataEncoder writes
// them: a boundary of 16 random characters after "----WebKitFormBoundary",
// and the header of each part, with its name and file name quoted and their
// quotes and line breaks percent-encoded.
//
// Upstream: third_party/blink/renderer/platform/network/form_data_encoder.cc

#ifndef MITTO_DOM_FORM_DATA_ENCODER_H_
#define MITTO_DOM_FORM_DATA_ENCODER_H_

#include <string>
#include <string_view>

namespace mitto::form_data_encoder {

std::string GenerateUniqueBoundaryString();

// "--boundary" and a line break, with "--" before the line break for the last.
void AddBoundaryToMultiPartHeader(std::string& buffer,
                                  std::string_view boundary,
                                  bool is_last_boundary = false);
// A boundary, and a part's Content-Disposition with `name`, in UTF-8.
void BeginMultiPartHeader(std::string& buffer,
                          std::string_view boundary,
                          std::string_view name);
// The part's file name, in UTF-8.
void AddFilenameToMultiPartHeader(std::string& buffer,
                                  std::string_view filename);
void AddContentTypeToMultiPartHeader(std::string& buffer,
                                     std::string_view mime_type);
// The empty line after a part's header.
void FinishMultiPartHeader(std::string& buffer);

}  // namespace mitto::form_data_encoder

#endif  // MITTO_DOM_FORM_DATA_ENCODER_H_
