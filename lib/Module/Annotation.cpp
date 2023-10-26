#include "klee/Module/Annotation.h"

#include "klee/Support/ErrorHandling.h"

#include <fstream>
#include <llvm/Support/raw_ostream.h>
#include <utility>

namespace klee {

static inline std::string toLower(const std::string &str) {
  std::string strLower;
  strLower.reserve(str.size());
  std::transform(str.begin(), str.end(), std::back_inserter(strLower), tolower);
  return strLower;
}

namespace Statement {

Unknown::Unknown(const std::string &str) {
  {
    const size_t firstColonPos = str.find(':');
    const size_t startOffset = firstColonPos + 1;
    const size_t secondColonPos = str.find(':', startOffset);
    const size_t offsetLength = (secondColonPos == std::string::npos)
                                    ? std::string::npos
                                    : secondColonPos - startOffset;

    rawAnnotation = str.substr(0, firstColonPos);
    if (firstColonPos == std::string::npos) {
      return;
    }
    rawOffset = str.substr(startOffset, offsetLength);
    if (secondColonPos != std::string::npos) {
      rawValue = str.substr(secondColonPos + 1, std::string::npos);
    }
  }

  for (size_t pos = 0; pos < rawOffset.size(); pos++) {
    switch (rawOffset[pos]) {
    case '*': {
      offset.emplace_back("*");
      break;
    }
    case '&': {
      offset.emplace_back("&");
      break;
    }
    case '[': {
      size_t posEndExpr = rawOffset.find(']', pos);
      if (posEndExpr == std::string::npos) {
        klee_error("Annotation: Incorrect offset format \"%s\"", str.c_str());
      }
      offset.push_back(rawOffset.substr(pos + 1, posEndExpr - 1 - pos));
      pos = posEndExpr;
      break;
    }
    default: {
      klee_warning("Annotation: Incorrect offset format \"%s\"", str.c_str());
      break;
    }
    }
  }
}

Unknown::~Unknown() = default;

Kind Unknown::getKind() const { return Kind::Unknown; }

const std::vector<std::string> &Unknown::getOffset() const { return offset; }
std::string Unknown::toString() const {
  if (rawValue.empty()) {
    if (rawOffset.empty()) {
      return rawAnnotation;
    } else {
      return rawAnnotation + ":" + rawOffset;
    }
  }
  return rawAnnotation + ":" + rawOffset + ":" + rawValue;
}

bool Unknown::operator==(const Unknown &other) const {
  if (this->getKind() == other.getKind()) {
    return toString() == other.toString();
  }
  return false;
}

/*
 * InitNull:*[5]:
 * {kind}:{offset}:{data}
 */

Deref::Deref(const std::string &str) : Unknown(str) {}

Kind Deref::getKind() const { return Kind::Deref; }

InitNull::InitNull(const std::string &str) : Unknown(str) {}

Kind InitNull::getKind() const { return Kind::InitNull; }

AllocSource::AllocSource(const std::string &str) : Unknown(str) {
  if (!std::all_of(rawValue.begin(), rawValue.end(), isdigit)) {
    klee_error("Annotation: Incorrect value format \"%s\"", rawValue.c_str());
  }
  if (rawValue.empty()) {
    value = AllocSource::Type::Alloc;
  } else {
    value = static_cast<Type>(std::stoi(rawValue));
  }
}

Kind AllocSource::getKind() const { return Kind::AllocSource; }

Free::Free(const std::string &str) : Unknown(str) {
  if (!std::all_of(rawValue.begin(), rawValue.end(), isdigit)) {
    klee_error("Annotation: Incorrect value format \"%s\"", rawValue.c_str());
  }
  if (rawValue.empty()) {
    value = Free::Type::Free_;
  } else {
    value = static_cast<Type>(std::stoi(rawValue));
  }
}

Kind Free::getKind() const { return Kind::Free; }

const std::map<std::string, Statement::Kind> StringToKindMap = {
    {"deref", Statement::Kind::Deref},
    {"initnull", Statement::Kind::InitNull},
    {"allocsource", Statement::Kind::AllocSource},
    {"freesource", Statement::Kind::Free},
    {"freesink", Statement::Kind::Free}};

inline Statement::Kind stringToKind(const std::string &str) {
  auto it = StringToKindMap.find(toLower(str));
  if (it != StringToKindMap.end()) {
    return it->second;
  }
  return Statement::Kind::Unknown;
}

Ptr stringToKindPtr(const std::string &str) {
  std::string statementStr = toLower(str.substr(0, str.find(':')));
  switch (stringToKind(statementStr)) {
  case Statement::Kind::Unknown:
    return std::make_shared<Unknown>(str);
  case Statement::Kind::Deref:
    return std::make_shared<Deref>(str);
  case Statement::Kind::InitNull:
    return std::make_shared<InitNull>(str);
  case Statement::Kind::AllocSource:
    return std::make_shared<AllocSource>(str);
  case Statement::Kind::Free:
    return std::make_shared<Free>(str);
  }
}

const std::map<std::string, Property> StringToPropertyMap{
    {"deterministic", Property::Deterministic},
    {"noreturn", Property::Noreturn},
};

inline Property stringToProperty(const std::string &str) {
  auto it = StringToPropertyMap.find(toLower(str));
  if (it != StringToPropertyMap.end()) {
    return it->second;
  }
  return Property::Unknown;
}

void from_json(const json &j, Ptr &statement) {
  if (!j.is_string()) {
    klee_error("Annotation: Incorrect statement format");
  }
  const std::string jStr = j.get<std::string>();
  statement = Statement::stringToKindPtr(jStr);
}

void from_json(const json &j, Property &property) {
  if (!j.is_string()) {
    klee_error("Annotation: Incorrect properties format");
  }
  const std::string jStr = j.get<std::string>();

  property = Statement::Property::Unknown;
  const auto propertyPtr = Statement::StringToPropertyMap.find(jStr);
  if (propertyPtr != Statement::StringToPropertyMap.end()) {
    property = propertyPtr->second;
  }
}

bool operator==(const Statement::Ptr &first, const Statement::Ptr &second) {
  if (first->getKind() != second->getKind()) {
    return false;
  }

  return *first.get() == *second.get();
}
} // namespace Statement

bool Annotation::operator==(const Annotation &other) const {
  return (functionName == other.functionName) &&
         (returnStatements == other.returnStatements) &&
         (argsStatements == other.argsStatements) &&
         (properties == other.properties);
}

AnnotationsMap parseAnnotationsJson(const json &annotationsJson) {
  AnnotationsMap annotations;
  for (auto &item : annotationsJson.items()) {
    Annotation annotation;
    annotation.functionName = item.key();

    const json &j = item.value();
    if (!j.is_object() || !j.contains("annotation") ||
        !j.contains("properties")) {
      klee_error("Annotation: Incorrect file format");
    }
    {
      std::vector<std::vector<Statement::Ptr>> tmp =
          j.at("annotation").get<std::vector<std::vector<Statement::Ptr>>>();

      if (tmp.empty()) {
        klee_error("Annotation: function \"%s\" should has return",
                   annotation.functionName.c_str());
      }
      annotation.returnStatements = tmp[0];
      annotation.argsStatements =
          std::vector<std::vector<Statement::Ptr>>(tmp.begin() + 1, tmp.end());
    }

    annotation.properties =
        j.at("properties").get<std::set<Statement::Property>>();
    annotations[item.key()] = annotation;
  }
  return annotations;
}

AnnotationsMap parseAnnotations(const std::string &path,
                                const llvm::Module *m) {
  if (path.empty()) {
    return {};
  }
  std::ifstream annotationsFile(path);
  if (!annotationsFile.good()) {
    klee_error("Annotation: Opening %s failed.", path.c_str());
  }
  {
    json annotationsJson = json::parse(annotationsFile, nullptr, false);
    if (annotationsJson.is_discarded()) {
      klee_error("Annotation: Parsing JSON %s failed.", path.c_str());
    }

    return parseAnnotationsJson(annotationsJson);
  }
}

} // namespace klee