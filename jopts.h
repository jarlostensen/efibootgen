#pragma once

#include <unordered_map>
#include <cstdarg>

// A *very* simple (but functional) options argument parser.
// Uses some C++ 17 features and std library containers. 
// 
// Arguments are in the form 
//      -a, --argument
//
// For usage see efibootgen.cpp
namespace jopts
{
    // constraints on parameters
    enum class option_constraint_t
    {
        // may or may not be present
        kOptional,
        // must be present
        kRequired,
    };
    // default behaviour
    enum class option_default_t
    {
        // false or empty
        kNotPresent,
        // true or provided (see add)
        kPresent,
    };
    // a limited subset of option types
    enum class option_type_t
    {
        // a flag is present or not present
        kFlag,
        // a path (not validated)
        kPath,
        // just text
        kText,
        // XxY
        kDimensions,
    };

    namespace detail
    {
        struct option_impl_t
        {
            option_impl_t() = default;
            option_impl_t(option_constraint_t constraint, option_type_t type, const std::string& opt_short, const std::string& opt_long, const std::string& about)
                : _short{ opt_short }
                , _long{ opt_long }
                , _about{ about }
                , _type{ type }
                , _constraint{ constraint }
            {}

            option_impl_t(option_impl_t&& rhs)
                : _short{ std::move(rhs._short) }
                , _long{ std::move(rhs._long) }
                , _about{ std::move(rhs._about) }
                , _type(rhs._type)
                , _constraint{ rhs._constraint }
            {}

            std::string         _short;
            std::string         _long;
            std::string         _about;
            option_type_t       _type;
            option_constraint_t       _constraint;
            bool                _present = false;

            union _value_t {
                std::string     _str = {};
                bool            _flag;
                _value_t() {}
                ~_value_t() {}
            } _value;
        };

        using option_vector_t = std::vector<option_impl_t>;
    }

    // handle and accessor for an option
    struct option_t
    {
        option_t() = default;
        explicit option_t(detail::option_vector_t* opt_vec, size_t idx)
            : _opt_vec{opt_vec}
            , _idx{idx}
        {}
        option_t(const option_t&) = default;
        option_t& operator=(const option_t&) = default;
        option_t(option_t&&) = default;
        option_t& operator=(option_t&&hs) = default;
        ~option_t() = default;
 
        template<typename T>
        System::status_or_t<T> as() const;

        template<>
        System::status_or_t<bool> as() const
        {
            auto* impl = &_opt_vec->at(_idx);
            if (impl->_type != option_type_t::kFlag)
            {
                return System::Code::NOT_FOUND;
            }
            return impl->_value._flag;
        }

        template<>
        System::status_or_t<std::string> as() const
        {
            auto* impl = &_opt_vec->at(_idx);
            if (impl->_type != option_type_t::kText
                &&
                impl->_type != option_type_t::kPath)
            {
                return System::Code::NOT_FOUND;
            }
            return impl->_value._str;
        }

        operator bool() const
        {
            auto* impl = &_opt_vec->at(_idx);
            return impl->_present;
        }

        detail::option_vector_t*  _opt_vec = nullptr;
        size_t                    _idx = 0;
    };

    // the actual parser
    struct option_parser_t
    {
        // add an option to the parser (before parsing).
        // examples:
        //  an optional flag, defaults to "false"
        //      add(kOptional, kFlag, "f,flag", "this is a flag", kNotPresent);
        //  an optional path, defaults to "."
        //      add(kOptional, kPath, "p,path", "this is a path to nowhere", kPresent, ".");
        // ...
        option_t    add(option_constraint_t constraint, option_type_t type, const std::string& shortLong, const std::string& about, option_default_t default_, ...)
        {
            const auto sl_delim = shortLong.find(',');
            assert(sl_delim != std::string::npos);

            const auto opt_short = shortLong.substr(0, sl_delim);
            const auto opt_long = shortLong.substr(sl_delim + 1);
            assert(opt_short.length() > 0 && opt_short.length() < opt_long.length());

            const auto si = _short.find(opt_short);
            assert(si == _short.end());

            const auto li = _long.find(opt_long);
            assert(li == _long.end());

            detail::option_impl_t opt{ constraint, type, opt_short, opt_long, about };
            if (constraint == option_constraint_t::kOptional)
            {
                switch (type)
                {
                case option_type_t::kFlag:
                    opt._value._flag = default_ == option_default_t::kPresent ? true : false;
                    break;
                case option_type_t::kPath:
                case option_type_t::kText:
                {
                    if (default_ == option_default_t::kPresent)
                    {
                        va_list defaults;
                        va_start(defaults, default_);
                        opt._value._str = va_arg(defaults, const char*);
                        va_end(defaults);
                    }
                }
                break;
                case option_type_t::kDimensions:
                default:;
                }
            }

            _options.emplace_back(std::move(opt));

            const auto i = _options.size()-1;
            _short[opt_short] = i;
            _long[opt_long] = i;

            return option_t{ &_options, i };
        }

        // parse the parameters.
        // if strict then any unknown argument causes the parser to fail
        // TODO: upper-lower case
        // TODO: more types
        System::status_or_t<bool> parse(int argc, char** argv, bool strict = false)
        {
            //ZZZ: perhaps too strict?
            if (_parsed)
            {
                return System::Code::ALREADY_EXISTS;
            }

            // always add this
            const auto res = add(option_constraint_t::kOptional, option_type_t::kFlag, "h,help", "about this application", option_default_t::kNotPresent);

            for (int n = 1; n < argc;)
            {
                auto* arg = argv[n];
                while (arg[0] && arg[0] != '-') ++arg;
                ++arg;
                detail::option_impl_t* opt = nullptr;
                if (arg[0] != '-')
                {
                    const auto si = _short.find(arg);
                    if (si != _short.end())
                    {
                        opt = &_options[si->second];
                    }
                    else
                    {
                        return System::Code::INVALID_ARGUMENT;
                    }
                }
                else
                {
                    const auto li = _long.find(++arg);
                    if (li != _long.end())
                    {
                        opt = &_options[li->second];
                    }
                    else if (strict)
                    {
                        return System::Code::INVALID_ARGUMENT;
                    }
                }

                if (opt)
                {
                    opt->_present = true;
                    switch (opt->_type)
                    {
                    case option_type_t::kFlag:
                    {
                        // flags have no arguments, they just exist...
                        opt->_present = true;
                    }
                    break;
                    case option_type_t::kPath:
                    case option_type_t::kText:
                    {
                        if (n == (argc - 1))
                        {
                            return System::Code::INVALID_ARGUMENT;
                        }
                        ++n;
                        opt->_value._str = argv[n];
                    }
                    break;
                    //TODO:
                    case option_type_t::kDimensions:
                    default:;
                    }
                }
                ++n;
            }

            // now check that we've got everything we need
            for (auto& opt : _options)
            {
                if (opt._constraint == option_constraint_t::kRequired && !opt._present)
                {
                    return System::Code::INVALID_ARGUMENT;
                }
            }

            return true;
        }

        // true if -h or --help found
        bool help_needed() const
        {
            // always the last option
            return _options[_options.size()-1]._present;
        }

        // print out information about our options
        std::ostream& print_about(std::ostream& os)
        {
            auto i = 0u;
            for (auto& opt : _options)
            {
                os << "-" << opt._short << ", --" << opt._long << "\t\t" << opt._about << "\n";
            }
            return os;
        }

        std::vector<detail::option_impl_t>          _options;
        std::unordered_map<std::string, size_t>     _short;
        std::unordered_map<std::string, size_t>     _long;
        bool                                        _parsed = false;
    };
}
