#include "RDM6300Wrapper.h"

const uint32_t RDM6300Wrapper::BAUD;  // ODR-use definition (see RDM6300Parser.cpp)

bool RDM6300Wrapper::read()
{
    while (port_.available() > 0) {
        int b = port_.read();
        if (b < 0) break;
        if (parser_.feed((uint8_t)b, millis())) {
            // Return straight away so a second card in the same burst isn't
            // lost; its bytes are still in the stream for the next call.
            RDM6300Parser::toHex(parser_.lastCard(), hex_);
            return true;
        }
    }
    return false;
}
