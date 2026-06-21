#pragma once

#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "ConfigInput.h"

namespace JCore {
namespace NS_PLAT {

class Config {
protected:
    std::string prefix;
    std::map<std::string, std::function<void(std::string const&)>> dispatcher;
public:
    std::map<std::string, std::function<std::string()>> recorder;
    Config() {};
    ~Config() {};

    virtual void overrideDefaultConfig(std::shared_ptr<ConfigInput> cfgs) final {
        for (auto &cfg : cfgs->cfgMap) {
            const std::string &entry = cfg.first;
            auto pos = entry.find('=');

            assert(pos != std::string::npos && pos > 0 && pos + 1 < entry.size());

            std::string cfg_name = entry.substr(0, pos);
            std::string cfg_value = entry.substr(pos + 1);

            bool updateCfg = parseConfig(cfg_name, cfg_value);
            if (updateCfg) {
                cfg.second++;
            }
        }
    }
    
    bool parseConfig(std::string const& cfg_name, std::string const& cfg_value) {
        if (cfg_name.substr(0, prefix.size()) == prefix) {
            assert(cfg_name[prefix.size()] == '.');
            auto it = dispatcher.find(cfg_name.substr(prefix.size()+1));
            if (it != dispatcher.end()) {
                it->second(cfg_value);
            } else {
                std::cerr << "Invalid config name: " << cfg_name << std::endl;
                exit(-1);
            }
            return true;
        }
        return false;
    }

    virtual std::string const& ParseString(std::string const& v) final { return v; }

    virtual uint64_t ParseInteger(std::string const& v) final { 
        if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) return stoull(v, nullptr, 16);
        else return stoull(v);
    }

    virtual bool ParseBoolean(std::string const& v) final {
        if (v == "false") return false;
        else if (v == "true") return true;
        else assert(0);
    }

    virtual void ParseIntVec(std::string const& v, std::vector<uint64_t>& array) final {
        std::size_t pos = 0;
        array.clear();
        std::string vv = v;
        while((pos = vv.find(':')) != std::string::npos) {
            array.push_back(stoull(vv.substr(0, pos)));
            vv.erase(0, pos+1);
        }
        array.push_back(stoull(vv.substr(0, pos)));
    }

    virtual void ParseStrVec(std::string const &v, std::vector<std::string> &array) final
    {
        std::size_t pos = 0;
        array.clear();
        std::string vv = v;
        while ((pos = vv.find(':')) != std::string::npos) {
            array.push_back(vv.substr(0, pos));
            vv.erase(0, pos + 1);
        }
        array.push_back(vv.substr(0, pos));
    }

    virtual std::string ParameterToStr(bool parameter) final
    {
        return std::to_string(parameter);
    }

    virtual std::string ParameterToStr(uint64_t parameter) final
    {
        return std::to_string(parameter);
    }

    virtual std::string ParameterToStr(std::vector<uint64_t> &parameter) final
    {
        std::stringstream oss;
        oss << "[";
        for (auto &it : parameter) {
            oss << it << ",";
        }
        oss << "]";
        return oss.str();
    }

    virtual std::string ParameterToStr(const std::string &parameter) final
    {
        return parameter;
    }

    virtual std::string ParameterToStr(std::vector<std::string> &parameter) final
    {
        std::stringstream oss;
        oss << "[";
        for (size_t i = 0; i < parameter.size(); ++i) {
            oss << parameter[i];
            if (i < parameter.size() - 1) {
                oss << ",";
            }
        }
        oss << "]";
        return oss.str();
    }

    virtual std::string DumpParameters() final
    {
        std::stringstream oss;
        oss << "[" << prefix << "]" << std::endl;
        for (auto &it : recorder) {
            oss << it.first << " ===== ";
            oss << it.second() << std::endl;
        }
        oss << std::endl;
        return oss.str();
    }

    virtual std::string GetCondfigValue(std::string name) final
    {
       std::stringstream oss;
        for (auto &it : recorder) {
            if(it.first == name) {
                oss << it.second() << std::endl;
                return oss.str();
            }
        }
        assert(0);
        return oss.str(); 
    }

};

} // namespace NS_PLAT
} // namespace JCore
