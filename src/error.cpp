#include "particle_codec/error.h"

namespace particle_codec {
    const char *errorName(ErrorCode code) {
        switch (code) {
            case ErrorCode::None:
                return "None";
            case ErrorCode::InvalidArgument:
                return "InvalidArgument";
            case ErrorCode::NoParticles:
                return "NoParticles";
            case ErrorCode::SyncNotFound:
                return "SyncNotFound";
            case ErrorCode::FrameTooShort:
                return "FrameTooShort";
            case ErrorCode::PayloadTooLong:
                return "PayloadTooLong";
            case ErrorCode::CrcMismatch:
                return "CrcMismatch";
            case ErrorCode::FrameDuplicate:
                return "FrameDuplicate";
            case ErrorCode::BufferOverflow:
                return "BufferOverflow";
            case ErrorCode::ImageLoadFailed:
                return "ImageLoadFailed";
            case ErrorCode::DecodeFailed:
                return "DecodeFailed";
        }
        return "Unknown";
    }
} // namespace particle_codec
