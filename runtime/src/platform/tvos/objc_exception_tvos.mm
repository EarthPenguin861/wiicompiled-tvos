// Describes an in-flight Objective-C exception for the runtime's terminate handler.
//
// The runtime installs its own std::terminate handler, which replaces the Objective-C runtime's
// "Terminating app due to uncaught exception" report. An NSException raised inside UIKit, SDL or
// Metal (for example an unrecognized selector) would otherwise reach the crash log only as an
// "unhandled non-std C++ exception" with no name, reason or stack. On Apple's 64-bit runtimes
// Objective-C exceptions travel as C++ exceptions, so the captured exception_ptr can be rethrown
// here and caught as an NSException.

#include "platform/tvos_app.h"

#import <Foundation/Foundation.h>

#include <exception>
#include <string>

namespace RuntimeTvos {

std::string DescribeObjCException(std::exception_ptr exception) noexcept {
    if (!exception) {
        return {};
    }
    try {
        @try {
            std::rethrow_exception(exception);
        } @catch (NSException* objcException) {
            NSMutableString* text = [NSMutableString
                stringWithFormat:@"Objective-C exception %@: %@", objcException.name, objcException.reason];
            NSArray<NSString*>* symbols = objcException.callStackSymbols;
            const NSUInteger frames = MIN(symbols.count, (NSUInteger)48);
            for (NSUInteger i = 0; i < frames; ++i) {
                [text appendFormat:@"\n  %@", symbols[i]];
            }
            const char* utf8 = text.UTF8String;
            return utf8 != nullptr ? std::string(utf8) : std::string("Objective-C exception (undescribable)");
        } @catch (id other) {
            const char* utf8 = [[other description] UTF8String];
            return std::string("Objective-C exception object: ") + (utf8 != nullptr ? utf8 : "?");
        }
    } catch (...) {
        // A C++ exception that is not an Objective-C object: nothing to add.
    }
    return {};
}

} // namespace RuntimeTvos
