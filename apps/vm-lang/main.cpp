//------------------------------------------------------------------------------
//
//   Copyright 2018-2019 Fetch.AI Limited
//
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.
//
//------------------------------------------------------------------------------

#include "core/byte_array/decoders.hpp"
#include "core/byte_array/encoders.hpp"
#include "core/commandline/cli_header.hpp"
#include "core/commandline/params.hpp"
#include "core/json/document.hpp"
#include "variant/variant.hpp"
#include "vm/analyser.hpp"
#include "vm/compiler.hpp"
#include "vm/module.hpp"
#include "vm/typeids.hpp"
#include "vm/vm.hpp"
#include "vm_modules/vm_factory.hpp"

#include <fstream>
#include <memory>
#include <sstream>
#include <streambuf>
#include <vector>

namespace {

using fetch::vm::Script;
using fetch::vm::Ptr;
using fetch::vm::VM;
using fetch::vm::String;
using fetch::vm::TypeId;
using fetch::vm_modules::VMFactory;
using fetch::variant::Variant;
using fetch::json::JSONDocument;
using fetch::byte_array::ConstByteArray;
using fetch::byte_array::FromHex;
using fetch::byte_array::ToHex;

class Parameters
{
public:
  using ParamParser = fetch::commandline::ParamsParser;
  using ArgList     = std::vector<char *>;
  using StringList  = std::vector<std::string>;

  void Parse(int argc, char **argv)
  {
    const std::string SEPARATOR{"--"};

    ArgList program_args{};
    ArgList script_args{};

    // first parameter is common between both argument set
    program_args.push_back(argv[0]);
    script_args.push_back(argv[0]);

    // loop through all the parameters
    ArgList *current_list = &program_args;
    for (int i = 1; i < argc; ++i)
    {
      if (SEPARATOR == argv[i])
      {
        // switch the list
        current_list = &script_args;
      }
      else
      {
        // add the string to the list
        current_list->push_back(argv[i]);
      }
    }

    // parse the program arguments
    program_params_.Parse(static_cast<int>(program_args.size()), program_args.data());

    // copy the script arguments
    for (auto const *s : script_args)
    {
      script_args_.emplace_back(s);
    }
  }

  ParamParser const &program() const
  {
    return program_params_;
  }

  StringList const &script() const
  {
    return script_args_;
  }

private:
  ParamParser program_params_{};
  StringList  script_args_{};
};

std::string ReadFileContents(std::string const &path)
{
  std::ifstream f(path.c_str());
  std::string   str{};

  if (!f.is_open())
  {
    return str;
  }

  // pre-allocate the string buffer
  f.seekg(0, std::ios::end);
  auto const size = static_cast<std::size_t>(f.tellg());
  if (size == 0)
  {
    return str;
  }

  str.reserve(size);
  f.seekg(0, std::ios::beg);

  // assign the contents
  str.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());

  return str;
}

// data
Parameters params;

int32_t Argc(VM *, TypeId)
{
  return static_cast<int32_t>(params.script().size());
}

Ptr<String> Argv(VM *vm, TypeId, int32_t index)
{
  return {new String{vm, params.script().at(static_cast<std::size_t>(index))}};
}

// placeholder class
struct System : public fetch::vm::Object
{
};

struct JsonStateMap : public fetch::vm::IoObserverInterface
{
public:
  // Construction / Destruction
  JsonStateMap()                     = default;
  JsonStateMap(JsonStateMap const &) = delete;
  JsonStateMap(JsonStateMap &&)      = delete;
  ~JsonStateMap() override           = default;

  /// @name Save and Restore Operations
  /// @{
  void LoadFromFile(char const *filename)
  {
    // read the contents of the file
    ConstByteArray file_contents{ReadFileContents(filename)};

    if (!file_contents.empty())
    {
      // parse the contents of the file
      JSONDocument document{file_contents};

      if (!document.root().IsObject())
      {
        throw std::runtime_error("JSON state file is not correct");
      }

      // load the file
      data_ = document.root();
    }
  }

  void SaveToFile(char const *filename)
  {
    std::ofstream file{filename};
    file << data_;
  }
  /// @}

  /// @name Io Observable Interface
  /// @{
  Status Read(std::string const &key, void *data, uint64_t &size) override
  {
    Status status{Status::ERROR};

    if (data_.Has(key))
    {
      auto const value = FromHex(data_[key].As<ConstByteArray>());

      status = Status::BUFFER_TOO_SMALL;
      if (size >= value.size())
      {
        auto *raw_data = reinterpret_cast<uint8_t *>(data);
        std::memcpy(raw_data, value.pointer(), value.size());

        status = Status::OK;
      }

      // update the output size
      size = value.size();
    }

    return status;
  }

  Status Write(std::string const &key, void const *data, uint64_t size) override
  {
    auto const *raw_data = reinterpret_cast<uint8_t const *>(data);

    // store the data key
    data_[key] = ToHex(ConstByteArray(raw_data, size));

    return Status::OK;
  }

  Status Exists(std::string const &key) override
  {
    return (data_.Has(key) ? Status::OK : Status::ERROR);
  }
  /// @}

  Variant const &data()
  {
    return data_;
  }

  // Operators
  JsonStateMap &operator=(JsonStateMap const &) = delete;
  JsonStateMap &operator=(JsonStateMap &&) = delete;

private:
  Variant data_{Variant::Object()};
};

}  // namespace

int main(int argc, char **argv)
{
  // parse the command line parameters
  params.Parse(argc, argv);

  // ensure the program has the correct number of args
  if (2u != params.program().arg_size())
  {
    std::cerr << "Usage: " << argv[0] << " [options] <filename> -- [script args]..." << std::endl;
    return 1;
  }

  // print the header
  fetch::commandline::DisplayCLIHeader("vm-lang");

  // load the contents of the script file
  auto const source = ReadFileContents(params.program().GetArg(1));

  auto script = std::make_unique<Script>();
  auto module = VMFactory::GetModule();

  // additional module bindings
  module->CreateClassType<System>("System")
      .CreateTypeFunction("Argc", &Argc)
      .CreateTypeFunction("Argv", &Argv);

  // attempt to compile the program
  auto errors = VMFactory::Compile(module, source, *script);

  // detect compilation errors
  if (!errors.empty())
  {
    std::cerr << "Failed to compile:\n";

    for (auto const &line : errors)
    {
      std::cerr << line << '\n';
    }

    return 1;
  }

  // create the VM instance
  auto vm = VMFactory::GetVM(module);

  std::string const data_path = params.program().GetParam("data", "");

  JsonStateMap state_map;
  vm->SetIOObserver(state_map);

  // restore and data file that is specified
  if (!data_path.empty())
  {
    state_map.LoadFromFile(data_path.c_str());
  }

  // Execute the requested function
  std::string        error;
  std::string        console;
  fetch::vm::Variant output;
  bool const         success =
      vm->Execute(*script, params.program().GetParam("func", "main"), error, console, output);

  // if there is any console output print it
  if (!console.empty())
  {
    std::cout << console << std::endl;
  }

  // display the error message if the script execution was not successful
  if (!success)
  {
    std::cerr << error << std::endl;
  }

  // save any specified data file
  if (!data_path.empty())
  {
    state_map.SaveToFile(data_path.c_str());
  }

  return (success) ? 0 : 1;
}