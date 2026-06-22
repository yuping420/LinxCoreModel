#include "error.h"

#include <execinfo.h>
#include <cstring>
#include <sstream>
#include <vector>
#include <functional>
#include <cxxabi.h>

namespace JCore {

class BacktraceImpl : public LazyValue<std::string> {
public:
    inline BacktraceImpl(size_t skipFrames, size_t maxFrames) : callStack_(maxFrames, 0) {
        skipFrames += 1;
        auto nrFrames = static_cast<size_t>(::backtrace(callStack_.data(), static_cast<int>(callStack_.size())));
        skipFrames = std::min(skipFrames, nrFrames);
        callStack_.erase(callStack_.begin(), callStack_.begin() + static_cast<ssize_t>(skipFrames));
        callStack_.resize(nrFrames - skipFrames);
    }

    void ParseFrame(std::stringstream &ss, char *line) const {
        auto funcName = strstr(line, "(");
        auto funcOffset = strstr(line, "+");
        if (funcName == nullptr || funcOffset == nullptr) {
            ss << line << '\n';
            return;
        }

        *funcName++ = '\0';
        *funcOffset++ = '\0';
        int status = 0;
        std::unique_ptr<char, std::function<void(char *)>> demangled(
            abi::__cxa_demangle(funcName, nullptr, nullptr, &status),
            /* deleter */ free);
        if (status == 0)
            funcName = demangled.get();
        ss << line << '(' << funcName << '+' << funcOffset << '\n';
    }

    const std::string &Get() const {
        return symbols_.Ensure([this]() -> std::string {
            auto strings = backtrace_symbols(callStack_.data(), callStack_.size());
            if (strings == nullptr) {
                return "Backtrace Failed";
            }
            std::stringstream ss;
            ss << "stack:\n";
            for (size_t i = 0; i < callStack_.size(); i++) {
                ParseFrame(ss, strings[i]);
            }
            free(strings);
            return ss.str();
        });
    }

private:
    mutable LazyShared<std::string> symbols_;
    std::vector<void *> callStack_;
};

Backtrace GetBacktrace(size_t skipFrames, size_t maxFrames) {
    return std::make_shared<BacktraceImpl>(BacktraceImpl{skipFrames, maxFrames});
}

const char *Error::what() const noexcept {
    return what_
        .Ensure([this]() -> std::string {
            std::stringstream ss;
            ss << msg_ << ", func " << func_ << ", file " << file_ << ":" << line_ << "\n";
            ss << backtrace_->Get();
            return ss.str();
        })
        .c_str();
}

} // namespace JCore
