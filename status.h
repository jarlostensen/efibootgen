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

    struct Status
    {
        Status() = default;
        Status(Code code)
            : _code{ code }
        {}
        Status(const Status&) = default;
        Status(Status&&) = default;
        Status& operator=(const Status&) = default;
        Status& operator=(Status&&) = default;
        ~Status() = default;

        operator bool() const
        {
            return _code == Code::OK;
        }

        [[nodiscard]]
        Code ErrorCode() const
        {
            return _code;
        }

        Code        _code = Code::OK;
    };

    template<typename T>
    struct StatusOr
    {
        StatusOr() = default;
        StatusOr(Code code)
            : _status{ code }
        {}
        StatusOr(const Status& status)
            : _status{ status }
        {}
        StatusOr(const T& val)
            : _status{ Code::OK },
            _value{ val }
        {}

        operator bool() const
        {
            return _status._code == Code::OK;
        }

        bool IsOk() const
        {
            return _status._code == Code::OK;
        }

        [[nodiscard]]
        T& Value()
        {
            return _value;
        }

        [[nodiscard]]
        int Error() const
        {
            return static_cast<int>(_status._code);
        }

        [[nodiscard]]
        Code ErrorCode() const
        {
            return _status._code;
        }

        //NOTE: if status is anything other than OK this is garbage
        [[nodiscard]]
        operator T& ()
        {
            return _value;
        }

        T           _value = {};
        Status      _status = Code::UNKNOWN;
    };

    template<typename T>
    struct StatusOr<T*>
    {
        StatusOr() = default;
        StatusOr(Code code)
            : _status{ code }
        {}
        StatusOr(const Status& status)
            : _status{ status }
        {}
        StatusOr(T* val)
            : _status{ Code::OK },
            _value{ val }
        {}

        bool IsOk() const
        {
            return _status._code == Code::OK;
        }

        [[nodiscard]]
        Code ErrorCode() const
        {
            return _status._code;
        }

        [[nodiscard]]
        T* Value()
        {
            return _value;
        }

        [[nodiscard]]
        operator T* ()
        {
            return _value;
        }

        //WHY: if these are ordered the other way around _value will be incorrectly set, the low order 32bits being untouched...but only here...        
        T* _value = nullptr;
        Status      _status = Code::UNKNOWN;
    };
}