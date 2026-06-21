#!/usr/bin/env python3
# coding: utf-8
import os
import pathlib
import logging

try:
    import tomllib
except ModuleNotFoundError:
    import toml as _toml

    def load_toml(path_to_toml):
        return _toml.load(path_to_toml)
else:
    def load_toml(path_to_toml):
        with open(path_to_toml, "rb") as f:
            return tomllib.load(f)

# 设置模块级别的 logger
logger = logging.getLogger(__name__)

generated_prefix = "generated_configs"

def delete_config(path_to_config):
    assert(os.path.isfile(path_to_config))
    if not path_to_config.exists():
        return
    with open(file = path_to_config, mode='r') as f:
        line = f.readline()
        if (line == '// generated from config.toml\n'):
            logger.info("delete: %s", path_to_config)
            if path_to_config.exists():
                path_to_config.unlink()

            path_cpp = path_to_config.with_suffix('.cpp')
            logger.info("delete: %s", path_cpp)
            if path_cpp.exists():
                path_cpp.unlink()

def process_toml(path_to_toml):
    if 'generic_soc' in str(path_to_toml):
        return
    assert(os.path.isfile(path_to_toml))
    d = load_toml(path_to_toml)

    logger.info("processing %s", path_to_toml)
    assert(len(list(d.keys()))==1)
    name = list(d.keys())[0]
    cfgs = d[name]
    dirname = os.path.dirname(path_to_toml)

    write_h(name, cfgs, dirname)
    write_cpp(name, cfgs, dirname)

def write_cpp(name, cfgs, dirname):
    def gen_disp_line(k, v):
        if isinstance(v, str):
            s = 'String'
        elif isinstance(v, bool):
            s = 'Boolean'
        elif isinstance(v, int):
            s = 'Integer'
        elif isinstance(v, list):
            if all(isinstance(x, str) for x in v) and len(v) != 0:
                s = 'StrVec'
            else:
                s = 'IntVec'

        if not isinstance(v, list):
            return f"        {{\"{k}\", [&](string v){{ {k} = Parse{s}(v); }}}},\n"
        else:
            return f"        {{\"{k}\", [&](string v){{ Parse{s}(v, {k}); }}}},\n"

    def gen_record_line(k, v):
        if isinstance(v, str):
            s = 'String'
        elif isinstance(v, bool):
            s = 'Boolean'
        elif isinstance(v, int):
            s = 'Integer'
        elif isinstance(v, list):
            s = 'IntVec'

        if s == 'String':
            return f"        {{\"{k}\", [&](){{ return \"{k} = \" + {k}; }}}},\n"
        else:
            return f"        {{\"{k}\", [&](){{ return \"{k} = \" + ParameterToStr({k}); }}}},\n"

    header = f"""#include "{name.lower()}_config.h"

namespace JCore {{
using namespace std;

{name}Config::{name}Config() {{
    Config::prefix = "{name.lower()}";
    Config::dispatcher = {{
"""
    middle = "    };\n"
    record = f"""
    Config::recorder = {{
"""
    footer = "    };\n}\n}"
    footer = "    };\n}\n} // namespace JCore\n"

    data = header
    for k in cfgs:
        data += gen_disp_line(k, cfgs[k])
    data += middle
    data += record
    for k in cfgs:
        data += gen_record_line(k, cfgs[k])
    data += footer

    output_file = os.path.join(dirname, name.lower() + '_config.cpp')
    if os.path.isfile(output_file):
        try:
            with open(output_file, 'r') as f:
                old_data = f.read()
                if old_data == data:
                    return
        except:
            pass
    with open(output_file, 'w') as f:
        f.write(data)

def write_h(name, cfgs, dirname):
    def gen_cfg_line(k,v):
        if isinstance(v, str):
            return f"    std::string {k} = \"{v}\";\n"
        elif isinstance(v, bool):
            return f"    bool {k} = {'true' if v else 'false'};\n"
        elif isinstance(v, int):
            return f"    uint64_t {k} = {v};\n"
        elif isinstance(v, list):
            if len(v) == 0:
                return f"    std::vector<uint64_t> {k} = {{}};\n"
            if all(isinstance(x, str) for x in v):
                items = '", "'.join(v)
                return f'    std::vector<std::string> {k} = {{"{items}"}};\n'
            return f"    std::vector<uint64_t> {k} = {str(v).replace('[','{').replace(']','}')};\n"
        else:
            assert(0)

    header = f"""// generated from config.toml
#pragma once

#include <cstdint>
#include <string>
#include "config.h"

namespace JCore {{
struct {name}Config : public NS_PLAT::Config {{
    {name}Config();
"""
    footer = "};\n} // namespace JCore\n"

    data = header
    for k in cfgs:
        data += gen_cfg_line(k, cfgs[k])
    data += footer

    output_file = os.path.join(dirname, name.lower() + '_config.h')
    if os.path.isfile(output_file):
        try:
            with open(output_file, 'r') as f:
                old_data = f.read()
                if old_data == data:
                    return
        except:
            pass
    with open(output_file, 'w') as f:
        f.write(data)

if __name__ == '__main__':
    logging.basicConfig(
        format='%(asctime)s - %(filename)s:%(lineno)d - %(levelname)s: %(message)s',
        level=logging.INFO
    )
    search_path = os.path.join(os.path.dirname(__file__), '../')
    toml_paths = pathlib.Path(search_path).rglob('*.toml')

    for t in toml_paths:
        process_toml(t)
