#include <algorithm>
#include <ranges>

#include "cmd.hpp"

namespace dcn::cmd
{
    template<>
    void ArgParser::addArg<bool>(std::string name, std::string desc)
    {
        _addArgDef(std::move(name), CommandLineArgDef::NArgs::Zero, CommandLineArgDef::Type::Bool, std::move(desc));
    }

    template<>
    void ArgParser::addArg<int>(std::string name, std::string desc)
    {
        _addArgDef(std::move(name), CommandLineArgDef::NArgs::One, CommandLineArgDef::Type::Int, std::move(desc));
    }

    template<>
    void ArgParser::addArg<std::vector<int>>(std::string name, std::string desc)
    {
        _addArgDef(std::move(name), CommandLineArgDef::NArgs::Many, CommandLineArgDef::Type::Int, std::move(desc));
    }

    template<>
    void ArgParser::addArg<unsigned int>(std::string name, std::string desc)
    {
        _addArgDef(std::move(name), CommandLineArgDef::NArgs::One, CommandLineArgDef::Type::Uint, std::move(desc));
    }

    template<>
    void ArgParser::addArg<std::vector<unsigned int>>(std::string name, std::string desc)
    {
        _addArgDef(std::move(name), CommandLineArgDef::NArgs::Many, CommandLineArgDef::Type::Uint, std::move(desc));
    }

    template<>
    void ArgParser::addArg<std::string>(std::string name, std::string desc)
    {
        _addArgDef(std::move(name), CommandLineArgDef::NArgs::One, CommandLineArgDef::Type::String, std::move(desc));
    }

    template<>
    void ArgParser::addArg<std::vector<std::string>>(std::string name, std::string desc)
    {
        _addArgDef(std::move(name), CommandLineArgDef::NArgs::Many, CommandLineArgDef::Type::String, std::move(desc));
    }

    template<>
    void ArgParser::addArg<float>(std::string name, std::string desc)
    {
        _addArgDef(std::move(name), CommandLineArgDef::NArgs::One, CommandLineArgDef::Type::Float, std::move(desc));
    }

    template<>
    void ArgParser::addArg<std::vector<float>>(std::string name, std::string desc)
    {
        _addArgDef(std::move(name), CommandLineArgDef::NArgs::Many, CommandLineArgDef::Type::Float, std::move(desc));
    }

    template<>
    void ArgParser::addArg<std::filesystem::path>(std::string name, std::string desc)
    {
        _addArgDef(std::move(name), CommandLineArgDef::NArgs::One, CommandLineArgDef::Type::Path, std::move(desc));
    }

    template<>
    void ArgParser::addArg<std::vector<std::filesystem::path>>(std::string name, std::string desc)
    {
        _addArgDef(std::move(name), CommandLineArgDef::NArgs::Many, CommandLineArgDef::Type::Path, std::move(desc));
    }


    void ArgParser::_addArgDef(std::string name, CommandLineArgDef::NArgs nargs, CommandLineArgDef::Type type, std::string desc)
    {
        if(name.empty())
        {
            spdlog::error("Argument name cannot be empty");
            std::exit(1);
        }

        if(type == CommandLineArgDef::Type::Unknown)
        {
            spdlog::error("Argument type cannot be unknown");
            std::exit(1);
        }

        if(type == CommandLineArgDef::Type::Bool)
        {
            if(nargs != CommandLineArgDef::NArgs::Zero)
            {
                spdlog::error("Bool arguments cannot have nargs > 0");
                std::exit(1);
            }
        }
        else
        {
            // any non-bool arg must not have 0 nargs
            if(nargs == CommandLineArgDef::NArgs::Zero)
            {
                spdlog::error("Non-bool arguments must have nargs > 0");
                std::exit(1);
            }
        }

        _args.emplace_back(CommandLineArgDef{
            .name = std::move(name),
            .desc = std::move(desc),
            .nargs = nargs,
            .type = type
        });
    }

    void ArgParser::parse(int argc, char** argv) 
    {
        std::vector<CommandLineArgDef>::const_iterator arg_it = _args.end();
        for(int i = 1; i < argc; ++i)
        {
            // try to find argument option
            const auto arg_name = std::string{argv[i]};
            arg_it = std::ranges::find_if(_args, [&arg_name](auto& a) { return a.name == arg_name; });

            if(arg_it == _args.end())
            {
                spdlog::error("Unknown argument \"{}\"", arg_name);
                std::exit(1);
            }

            // we've found args option

            // if it's a bool arg set it and move on
            if(arg_it->type == CommandLineArgDef::Type::Bool)
            {
                _values[arg_name] = true;
                continue;
            }

            // if no values are expected, move on
            if(arg_it->nargs == CommandLineArgDef::NArgs::Zero)
            {
                continue;
            }

            // one or many values are expected

            // parse values
            int parsed_values = 0;
            for(int val_id = i + 1; val_id < argc; ++val_id)
            {
                const std::string value_str = argv[val_id];

                // check whether it's already another argument
                if(std::ranges::find_if(_args, [&value_str](auto& a){return a.name == value_str;}) != _args.end())
                {
                    break;
                }

                // not another argument so it's a value
                switch(arg_it->nargs)
                {
                    case CommandLineArgDef::NArgs::Zero:
                        spdlog::error("Argument \"{}\" expects no values", arg_name);
                        std::exit(1);
                    
                    case CommandLineArgDef::NArgs::One:
                        switch(arg_it->type)
                        {
                            case CommandLineArgDef::Type::Bool:
                                spdlog::error("Bool arguments cannot have nargs > 0");
                                std::exit(1);

                            case CommandLineArgDef::Type::Int:
                                try
                                {
                                    _values[arg_name] = std::stoi(value_str);
                                    ++parsed_values;
                                }
                                catch(const std::exception&)
                                {
                                    spdlog::error("Parsing arg \"{}\", of type int, with value \"{}\" failed",arg_name, value_str);
                                    std::exit(1);
                                }
                                break;

                            case CommandLineArgDef::Type::Uint:
                                try
                                {
                                    _values[arg_name] = (unsigned int)(std::stoul(value_str));
                                    ++parsed_values;
                                }
                                catch(const std::exception&)
                                {
                                    spdlog::error("Parsing arg \"{}\", of type unsigned int, with value \"{}\" failed",arg_name, value_str);
                                    std::exit(1);
                                }
                                break;

                            case CommandLineArgDef::Type::String:
                                _values[arg_name] = value_str;
                                ++parsed_values;
                                break;
                            
                            case CommandLineArgDef::Type::Float:
                                try
                                {
                                    _values[arg_name] = std::stof(value_str);
                                    ++parsed_values;
                                }
                                catch(const std::exception&)
                                {
                                    spdlog::error("Parsing arg \"{}\", of type float, with value \"{}\" failed",arg_name, value_str);
                                    std::exit(1);
                                }
                                break;
                            
                            case CommandLineArgDef::Type::Path:
                                _values[arg_name] = std::filesystem::path(value_str);
                                ++parsed_values;
                                break;
                            
                            default:
                                spdlog::error("Invalid type");
                                std::exit(1);
                        }

                        break;
                    
                    case CommandLineArgDef::NArgs::Many:
                        switch(arg_it->type)
                        {
                            case CommandLineArgDef::Type::Bool:
                                spdlog::error("Bool arguments cannot have nargs > 0");
                                std::exit(1);

                            case CommandLineArgDef::Type::Int:
                                try
                                {
                                    if(_values.find(arg_name) == _values.end())
                                    {
                                        _values[arg_name] = std::vector<int>();
                                    }
                                    std::get<std::vector<int>>(_values.at(arg_name)).emplace_back(std::stoi(value_str));
                                    ++parsed_values;
                                }
                                catch(const std::exception&)
                                {
                                    spdlog::error("Parsing of arg \"{}\" with value \"{}\" failed",arg_name, value_str);
                                    std::exit(1);
                                }
                                break;

                            case CommandLineArgDef::Type::Uint:
                                try
                                {
                                    if(_values.find(arg_name) == _values.end())
                                    {
                                        _values[arg_name] = std::vector<unsigned int>();
                                    }
                                    std::get<std::vector<unsigned int>>(_values.at(arg_name)).emplace_back((unsigned int)(std::stoul(value_str)));
                                    ++parsed_values;
                                }
                                catch(const std::exception&)
                                {
                                    spdlog::error("Parsing of arg \"{}\" with value \"{}\" failed",arg_name, value_str);
                                    std::exit(1);
                                }
                                break;
                            
                            case CommandLineArgDef::Type::String:
                                if(_values.find(arg_name) == _values.end())
                                {
                                    _values[arg_name] = std::vector<std::string>();
                                }
                                std::get<std::vector<std::string>>(_values.at(arg_name)).emplace_back(value_str);
                                ++parsed_values;
                                break;
                            
                            case CommandLineArgDef::Type::Float:
                                try
                                {
                                    if(_values.find(arg_name) == _values.end())
                                    {
                                        _values[arg_name] = std::vector<float>();
                                    }
                                    std::get<std::vector<float>>(_values.at(arg_name)).emplace_back(std::stof(value_str));
                                    ++parsed_values;
                                }
                                catch(const std::exception&)
                                {
                                    spdlog::error("Parsing of arg \"{}\" with value \"{}\" failed",arg_name, value_str);
                                    std::exit(1);
                                }
                                break;
                            
                            case CommandLineArgDef::Type::Path:
                                if(_values.find(arg_name) == _values.end())
                                {
                                    _values[arg_name] = std::vector<std::filesystem::path>();
                                }
                                std::get<std::vector<std::filesystem::path>>(_values.at(arg_name)).emplace_back(std::filesystem::path(value_str));
                                ++parsed_values;
                                break;

                            default:
                                spdlog::error("Invalid type");
                                std::exit(1);
                        }
                        break;
                    
                    default:
                        spdlog::error("Invalid nargs value");
                        std::exit(1);
                }

                if(arg_it->nargs == CommandLineArgDef::NArgs::One)
                {
                    // we only need one value
                    break;
                }
            }

            if(parsed_values == 0)
            {
                spdlog::error("Missing values for argument \"{}\"", arg_name);
                std::exit(1);
            }

            i += parsed_values;
        }
    }

    std::string ArgParser::constructHelpMessage() const
    {
        std::string help_message = "Usage: ";
        for(const auto& arg : _args)
        {
            if(arg.type == CommandLineArgDef::Type::Bool)
            {
                help_message += "[" + arg.name + "]";
            }
            else
            {
                help_message += "[" + arg.name + " <value>]";
            }
        }

        std::vector<std::pair<std::string, std::string>> msgs;

        // calculate max length for formating purposes 
        for(const auto& arg : _args)
        {
            if(arg.type == CommandLineArgDef::Type::Bool)
            {
                msgs.emplace_back(arg.name, arg.desc);
            }
            else
            {
                msgs.emplace_back(arg.name + " <value>", arg.desc);
            }
        }
        const int max_len = static_cast<int>(std::ranges::max(msgs, [](auto& a, auto& b){return a.first.length() < b.first.length();}).first.length());
        help_message += "\n";
        for(const auto& msg : msgs)
        {
            help_message += "  " +  msg.first;
            // add spaces to align 
            for(int i = 0; i < max_len - (int)msg.first.length() + 1; ++i)
                help_message += " ";

            help_message += msg.second + "\n"; 
        }

        return help_message;
    }
}