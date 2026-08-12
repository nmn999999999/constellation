#pragma once

#include <string>
#include <utility>

namespace particle_codec {

    // Error codes for the whole library. Data-dependent failures (decode,
    // frame assembly) are reported through Result<T> / ErrorInfo instead of
    // silent nullopt; programmer errors (invalid arguments) throw exceptions
    // with a descriptive message.
    enum class ErrorCode {
        None = 0,
        InvalidArgument,   // bad parameters (grid size, frame length, ...)
        NoParticles,       // no particles supplied for decode
        SyncNotFound,      // 0xAA55AA55 sync word not found
        FrameTooShort,     // recovered data shorter than a frame header
        PayloadTooLong,    // declared payload exceeds grid capacity
        CrcMismatch,       // CRC32 verification failed
        FrameDuplicate,    // assembler: sequence number already buffered
        BufferOverflow,    // assembler: buffer limit reached
        ImageLoadFailed,   // image could not be loaded (CLI layer)
        DecodeFailed,      // generic decode failure
    };

    // Human-readable short name for an error code, e.g. "CrcMismatch".
    const char *errorName(ErrorCode code);

    struct ErrorInfo {
        ErrorCode code = ErrorCode::None;
        std::string message;

        bool ok() const { return code == ErrorCode::None; }
        explicit operator bool() const { return ok(); }
    };

    inline ErrorInfo makeError(ErrorCode code, std::string message) {
        return ErrorInfo{code, std::move(message)};
    }

    // Result<T> is the error-safe carrier for value-returning operations:
    // either a value, or an ErrorInfo describing why the operation failed.
    // Check ok() before calling value()/takeValue().
    template <typename T>
    class Result {
    public:
        static Result success(T value) {
            Result r;
            r.value_ = std::move(value);
            r.hasValue_ = true;
            return r;
        }

        static Result failure(ErrorInfo error) {
            Result r;
            r.error_ = std::move(error);
            return r;
        }

        bool ok() const { return hasValue_; }
        explicit operator bool() const { return hasValue_; }

        const T &value() const { return value_; }
        T &value() { return value_; }
        T takeValue() { return std::move(value_); }

        const ErrorInfo &error() const { return error_; }

    private:
        T value_{};
        bool hasValue_ = false;
        ErrorInfo error_;
    };

    // Result<void> for operations that report success/failure only.
    template <>
    class Result<void> {
    public:
        static Result success() { return Result(); }

        static Result failure(ErrorInfo error) {
            Result r;
            r.error_ = std::move(error);
            return r;
        }

        bool ok() const { return error_.ok(); }
        explicit operator bool() const { return ok(); }
        const ErrorInfo &error() const { return error_; }

    private:
        ErrorInfo error_;
    };

} // namespace particle_codec
