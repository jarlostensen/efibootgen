#pragma once

#include <map>
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
        // just text left open to interpretation
        kText,
    };

    namespace detail
    {
        struct option_impl_t
        {
            option_impl_t() = default;
            option_impl_t(option_constraint_t constraint, option_type_t type, 
                const std::string& opt_short, 
                const std::string& opt_long, 
                const std::string& about)
                : _short{ opt_short }
                , _long{ opt_long }
                , _about{ about }
                , _type{ type }
                , _constraint{ constraint }
            {}
            option_impl_t(option_impl_t&& rhs) = default;
            
            std::string         _short;
            std::string         _long;
            std::string         _about;
            option_type_t       _type;
            option_constraint_t _constraint;
            bool                _present = false;
            std::string         _str = {};
        };

        using option_vector_t = std::vector<option_impl_t>;
    }

    // handle and accessor for an option
    struct option_t
    {
        option_t() = default;
        explicit option_t(detail::option_vector_t* opt_vec, size_t idx)
            : _opt_vec{ opt_vec }
            , _idx{ idx }
        {}
        option_t(const option_t&) = default;
        option_t& operator=(const option_t&) = default;
        option_t(option_t&&) = default;
        option_t& operator=(option_t&& hs) = default;
        ~option_t() = default;

        template<typename T>
        T as() const;

        template<>
        bool as() const
        {
            auto* impl = &_opt_vec->at(_idx);
            assert(impl->_type == option_type_t::kFlag);
            return impl->_present;
        }

        template<>
        std::string as() const
        {
            auto* impl = &_opt_vec->at(_idx);
            assert(impl->_type == option_type_t::kText);
            return impl->_str;
        }

        template<>
        const std::string& as() const
        {
            auto* impl = &_opt_vec->at(_idx);
            assert(impl->_type == option_type_t::kText);
            return impl->_str;
        }

        template<>
        std::string_view as() const
        {
            auto* impl = &_opt_vec->at(_idx);
            assert(impl->_type == option_type_t::kText);
            return impl->_str;
        }

        operator bool() const
        {
            auto* impl = &_opt_vec->at(_idx);
            return impl->_present;
        }

        detail::option_vector_t* _opt_vec = nullptr;
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

            auto opt_short = shortLong.substr(0, sl_delim);
            auto opt_long = shortLong.substr(sl_delim + 1);
            assert(opt_short.length() > 0 && opt_short.length() < opt_long.length());

            normalise_case(opt_short, opt_short);
            const auto si = _short.find(opt_short);
            assert(si == _short.end());

            normalise_case(opt_long, opt_long);
            const auto li = _long.find(opt_long);
            assert(li == _long.end());

            detail::option_impl_t opt{ constraint, type, opt_short, opt_long, about };
            if (constraint == option_constraint_t::kOptional)
            {
                switch (type)
                {
                case option_type_t::kFlag:
                    opt._present = (default_ == option_default_t::kPresent ? true : false);
                    break;
                case option_type_t::kText:
                {
                    if (default_ == option_default_t::kPresent)
                    {
                        va_list defaults;
                        va_start(defaults, default_);
                        opt._str = va_arg(defaults, const char*);
                        va_end(defaults);
                    }
                }
                break;
                default:;
                }
            }

            _options.emplace_back(std::move(opt));

            const auto i = _options.size() - 1;
            _short[opt_short] = i;
            _long[opt_long] = i;

            return option_t{ &_options, i };
        }

        // parse the parameters.
        // if strict then any unknown argument causes the parser to fail
        // returns status or number of matched arguments found
        //
        System::status_or_t<size_t> parse(int argc, char** argv, bool strict = false)
        {
            //ZZZ: perhaps too strict?
            if (_parsed)
            {
                return System::Code::ALREADY_EXISTS;
            }

            // always add this
            const auto res = add(option_constraint_t::kOptional, option_type_t::kFlag, "h,help", "about this application", option_default_t::kNotPresent);

            auto arg_counter = 0u;

            for (int n = 1; n < argc;)
            {
                auto* arg = argv[n];
                while (arg[0] && arg[0] != '-') ++arg;
                ++arg;
                detail::option_impl_t* opt = nullptr;
                if (arg[0] != '-')
                {
                    std::string_view uc_opt = arg;
                    normalise_case(uc_opt, arg);
                    
                    const auto si = _short.find(uc_opt);
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
                    std::string_view uc_opt = ++arg;
                    normalise_case(uc_opt, arg);

                    const auto li = _long.find(uc_opt);
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
                    ++arg_counter;
                    opt->_present = true;
                    switch (opt->_type)
                    {
                    case option_type_t::kText:
                    {
                        if (n == (argc - 1))
                        {
                            return System::Code::INVALID_ARGUMENT;
                        }
                        ++n;
                        opt->_str = argv[n];
                    }
                    break;
                    default:;
                    }
                }
                ++n;
            }

            if (arg_counter)
            {
                // now check that we've got everything we need
                for (auto& opt : _options)
                {
                    if (opt._constraint == option_constraint_t::kRequired && !opt._present)
                    {
                        return System::Code::INVALID_ARGUMENT;
                    }
                }
                _parsed = true;
            }

            return arg_counter;
        }

        // true if -h or --help found
        bool help_needed() const
        {
            // always the last option
            return _options[_options.size() - 1]._present;
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

    private:
        void normalise_case(std::string_view view, char* arg)
        {
            std::transform(view.begin(), view.end(), arg, ::tolower);
        }

        void normalise_case(std::string& dest , const std::string& src)
        {
            std::transform(src.begin(), src.end(), dest.begin(), ::tolower);
        }

        std::vector<detail::option_impl_t>              _options;
        //NOTE: using map here *just* for heterogenuous lookups (so that we can use string_views more effectively)
        std::map<std::string, size_t, std::less<>>      _short;
        std::map<std::string, size_t, std::less<>>      _long;
        bool                                            _parsed = false;
    };
}
