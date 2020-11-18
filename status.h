#pragma once

#include <ostream>

namespace System
{
    // inspired by http://www.furidamu.org/blog/2017/01/28/error-handling-with-statusor/
    enum class Code : int
    {
        OK = 0,
        CANCELLED = 1,
        UNKNOWN = 2,
        INVALID_ARGUMENT = 3,
        DEADLINE_EXCEEDED = 4,
        NOT_FOUND = 5,
        ALREADY_EXISTS = 6,
        PERMISSION_DENIED = 7,
        UNAUTHENTICATED = 16,
        RESOURCE_EXHAUSTED = 8,
        FAILED_PRECONDITION = 9,
        ABORTED = 10,
        OUT_OF_RANGE = 11,
        UNIMPLEMENTED = 12,
        INTERNAL = 13,
        UNAVAILABLE = 14,
        DATA_LOSS = 15,
    };

    std::ostream& operator<<(std::ostream& os, Code code)
    {
#define _SYSTEM_OS_OUT_CODE(codeName)\
    case System::Code::##codeName:\
    os<< #codeName << " (" << static_cast<int>(System::Code::##codeName) << ")";\
    break

        switch(code)
        {
        _SYSTEM_OS_OUT_CODE(OK);
        _SYSTEM_OS_OUT_CODE(CANCELLED);
        _SYSTEM_OS_OUT_CODE(INVALID_ARGUMENT);
        _SYSTEM_OS_OUT_CODE(DEADLINE_EXCEEDED);
        _SYSTEM_OS_OUT_CODE(NOT_FOUND);
        _SYSTEM_OS_OUT_CODE(ALREADY_EXISTS);
        _SYSTEM_OS_OUT_CODE(PERMISSION_DENIED);
        _SYSTEM_OS_OUT_CODE(UNAUTHENTICATED);
        _SYSTEM_OS_OUT_CODE(RESOURCE_EXHAUSTED);
        _SYSTEM_OS_OUT_CODE(FAILED_PRECONDITION);
        _SYSTEM_OS_OUT_CODE(ABORTED);
        _SYSTEM_OS_OUT_CODE(OUT_OF_RANGE);
        _SYSTEM_OS_OUT_CODE(UNIMPLEMENTED);
        _SYSTEM_OS_OUT_CODE(INTERNAL);
        _SYSTEM_OS_OUT_CODE(UNAVAILABLE);
        _SYSTEM_OS_OUT_CODE(DATA_LOSS);
        default:;
        }

        return os;
    }

    struct status_t
    {
        status_t() = default;
        status_t(Code code)
            : _code{ code }
        {}
        status_t(const status_t&) = default;
        status_t(status_t&&) = default;
        status_t& operator=(const status_t&) = default;
        status_t& operator=(status_t&&) = default;
        ~status_t() = default;

        operator bool() const
        {
            return _code == Code::OK;
        }

        [[nodiscard]]
        Code error_code() const
        {
            return _code;
        }

        Code        _code = Code::OK;
    };

    template<typename T>
    struct status_or_t
    {
        status_or_t() = default;
        status_or_t(Code code)
            : _status{ code }
        {}
        status_or_t(const status_t& status)
            : _status{ status }
        {}
        status_or_t(const T& val)
            : _status{ Code::OK },
            _value{ val }
        {}
        status_or_t& operator=(const status_or_t& rhs)
        {
            _status = rhs._status;
            _value = rhs._value;
            return *this;
        }
        status_or_t(const status_or_t& rhs)
            : _status{rhs._status}
            , _value{ rhs._value }
        {}
        status_or_t& operator=(status_or_t&& rhs)
        {
            _status = rhs._status;
            _value = std::move(rhs._value);
            return *this;
        }

        operator bool() const
        {
            return _status._code == Code::OK;
        }

        bool is_ok() const
        {
            return _status._code == Code::OK;
        }

        [[nodiscard]]
        T value()
        {
            return _value;
        }

        [[nodiscard]]
        T& ref()
        {
            return _value;
        }

        [[nodiscard]]
        const T& cref() const
        {
            return _value;
        }

        [[nodiscard]]
        int error() const
        {
            return static_cast<int>(_status._code);
        }

        [[nodiscard]]
        Code error_code() const
        {
            return _status._code;
        }

        T           _value = {};
        status_t      _status = Code::UNKNOWN;
    };

    template<typename T>
    struct status_or_t<T*>
    {
        status_or_t() = default;
        status_or_t(Code code)
            : _status{ code }
        {}
        status_or_t(const status_t& status)
            : _status{ status }
        {}
        status_or_t(T* val)
            : _status{ Code::OK },
            _value{ val }
        {}

        operator bool() const
        {
            return _status._code == Code::OK;
        }

        bool is_ok() const
        {
            return _status._code == Code::OK;
        }

        [[nodiscard]]
        Code error_code() const
        {
            return _status._code;
        }

        [[nodiscard]]
        T* value()
        {
            return _value;
        }

        //WHY: if these are ordered the other way around _value will be incorrectly set, the low order 32bits being untouched...but only here...        
        T* _value = nullptr;
        status_t      _status = Code::UNKNOWN;
    };
}