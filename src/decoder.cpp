#include <iostream>
#include <fstream>

#include "jpg.h"

// SOF specifies frame type, dimensions, and number of color components
void readStartOfFrame(std::ifstream& inFile, Header* const header) {
    std::cout << "Reading SOF Marker\n";
    if (header->numComponents != 0) {
        std::cout << "Error - Multiple SOFs detected\n";
        header->valid = false;
        return;
    }

    uint length = (inFile.get() << 8) + inFile.get();

    byte precision = inFile.get();
    if (precision != 8) {
        std::cout << "Error - Invalid precision: " << (uint)precision << '\n';
        header->valid = false;
        return;
    }

    header->height = (inFile.get() << 8) + inFile.get();
    header->width = (inFile.get() << 8) + inFile.get();
    if (header->height == 0 || header->width == 0) {
        std::cout << "Error - Invalid dimensions\n";
        header->valid = false;
        return;
    }
    header->mcuHeight = (header->height + 7) / 8;
    header->mcuWidth = (header->width + 7) / 8;
    header->mcuHeightReal = header->mcuHeight;
    header->mcuWidthReal = header->mcuWidth;

    header->numComponents = inFile.get();
    if (header->numComponents == 4) {
        std::cout << "Error - CMYK color mode not supported\n";
        header->valid = false;
        return;
    }
    if (header->numComponents == 0) {
        std::cout << "Error - Number of color components must not be zero\n";
        header->valid = false;
        return;
    }
    for (uint i = 0; i < header->numComponents; ++i) {
        byte componentID = inFile.get();
        // component IDs are usually 1, 2, 3 but rarely can be seen as 0, 1, 2
        // always force them into 1, 2, 3 for consistency
        if (componentID == 0) {
            header->zeroBased = true;
        }
        if (header->zeroBased) {
            componentID += 1;
        }
        if (componentID == 4 || componentID == 5) {
            std::cout << "Error - YIQ color mode not supported\n";
            header->valid = false;
            return;
        }
        if (componentID == 0 || componentID > 3) {
            std::cout << "Error - Invalid component ID: " << (uint)componentID << '\n';
            header->valid = false;
            return;
        }
        ColorComponent* component = &header->colorComponents[componentID - 1];
        if (component->used) {
            std::cout << "Error - Duplicate color component ID\n";
            header->valid = false;
            return;
        }
        component->used = true;
        byte samplingFactor = inFile.get();
        component->horizontalSamplingFactor = samplingFactor >> 4;
        component->verticalSamplingFactor = samplingFactor & 0x0F;
        if (componentID == 1) {
            if ((component->horizontalSamplingFactor != 1 && component->horizontalSamplingFactor != 2) ||
                (component->verticalSamplingFactor != 1 && component->verticalSamplingFactor != 2)) {
                std::cout << "Error - Sampling factors not supported\n";
                header->valid = false;
                return;
            }
            if (component->horizontalSamplingFactor == 2 && header->mcuWidth % 2 == 1) {
                header->mcuWidthReal += 1;
            }
            if (component->verticalSamplingFactor == 2 && header->mcuHeight % 2 == 1) {
                header->mcuHeightReal += 1;
            }
            header->horizontalSamplingFactor = component->horizontalSamplingFactor;
            header->verticalSamplingFactor = component->verticalSamplingFactor;
        }
        else {
            if (component->horizontalSamplingFactor != 1 || component->verticalSamplingFactor != 1) {
                std::cout << "Error - Sampling factors not supported\n";
                header->valid = false;
                return;
            }
        }
        component->quantizationTableID = inFile.get();
        if (component->quantizationTableID > 3) {
            std::cout << "Error - Invalid quantization table ID in frame components\n";
            header->valid = false;
            return;
        }
    }
    if (length - 8 - (3 * header->numComponents) != 0) {
        std::cout << "Error - SOF invalid\n";
        header->valid = false;
    }
}

// DQT contains one or more quantization tables
void readQuantizationTable(std::ifstream& inFile, Header* const header) {
    std::cout << "Reading DQT Marker\n";
    int length = (inFile.get() << 8) + inFile.get();
    length -= 2;

    while (length > 0) {
        byte tableInfo = inFile.get();
        length -= 1;
        byte tableID = tableInfo & 0x0F;

        if (tableID > 3) {
            std::cout << "Error - Invalid quantization table ID: " << (uint)tableID << '\n';
            header->valid = false;
            return;
        }
        header->quantizationTables[tableID].set = true;

        if (tableInfo >> 4 != 0) {
            for (uint i = 0; i < 64; ++i) {
                header->quantizationTables[tableID].table[zigZagMap[i]] = (inFile.get() << 8) + inFile.get();
            }
            length -= 128;
        }
        else {
            for (uint i = 0; i < 64; ++i) {
                header->quantizationTables[tableID].table[zigZagMap[i]] = inFile.get();
            }
            length -= 64;
        }
    }

    if (length != 0) {
        std::cout << "Error - DQT invalid\n";
        header->valid = false;
    }
}

// DHT contains one or more Huffman tables
void readHuffmanTable(std::ifstream& inFile, Header* const header) {
    std::cout << "Reading DHT Marker\n";
    int length = (inFile.get() << 8) + inFile.get();
    length -= 2;

    while (length > 0) {
        byte tableInfo = inFile.get();
        byte tableID = tableInfo & 0x0F;
        bool ACTable = tableInfo >> 4;

        if (tableID > 3) {
            std::cout << "Error - Invalid Huffman table ID: " << (uint)tableID << '\n';
            header->valid = false;
            return;
        }

        HuffmanTable* hTable;
        if (ACTable) {
            hTable = &header->huffmanACTables[tableID];
        }
        else {
            hTable = &header->huffmanDCTables[tableID];
        }
        hTable->set = true;

        hTable->offsets[0] = 0;
        uint allSymbols = 0;
        for (uint i = 1; i <= 16; ++i) {
            allSymbols += inFile.get();
            hTable->offsets[i] = allSymbols;
        }
        if (allSymbols > 162) {
            std::cout << "Error - Too many symbols in Huffman table\n";
            header->valid = false;
            return;
        }

        for (uint i = 0; i < allSymbols; ++i) {
            hTable->symbols[i] = inFile.get();
        }

        length -= 17 + allSymbols;
    }
    if (length != 0) {
        std::cout << "Error - DHT invalid\n";
        header->valid = false;
    }
}

// SOS contains color component info for the next scan
void readStartOfScan(std::ifstream& inFile, Header* const header) {
    std::cout << "Reading SOS Marker\n";
    if (header->numComponents == 0) {
        std::cout << "Error - SOS detected before SOF\n";
        header->valid = false;
        return;
    }

    uint length = (inFile.get() << 8) + inFile.get();

    for (uint i = 0; i < header->numComponents; ++i) {
        header->colorComponents[i].used = false;
    }

    // the number of components in the next scan might not be all
    //   components in the image
    byte numComponents = inFile.get();
    for (uint i = 0; i < numComponents; ++i) {
        byte componentID = inFile.get();
        // component IDs are usually 1, 2, 3 but rarely can be seen as 0, 1, 2
        if (header->zeroBased) {
            componentID += 1;
        }
        if (componentID > header->numComponents) {
            std::cout << "Error - Invalid color component ID: " << (uint)componentID << '\n';
            header->valid = false;
            return;
        }
        ColorComponent* component = &header->colorComponents[componentID - 1];
        if (component->used) {
            std::cout << "Error - Duplicate color component ID: " << (uint)componentID << '\n';
            header->valid = false;
            return;
        }
        component->used = true;

        byte huffmanTableIDs = inFile.get();
        component->huffmanDCTableID = huffmanTableIDs >> 4;
        component->huffmanACTableID = huffmanTableIDs & 0x0F;
        if (component->huffmanDCTableID > 3) {
            std::cout << "Error - Invalid Huffman DC table ID: " << (uint)component->huffmanDCTableID << '\n';
            header->valid = false;
            return;
        }
        if (component->huffmanACTableID > 3) {
            std::cout << "Error - Invalid Huffman AC table ID: " << (uint)component->huffmanACTableID << '\n';
            header->valid = false;
            return;
        }
    }

    header->startOfSelection = inFile.get();
    header->endOfSelection = inFile.get();
    byte successiveApproximation = inFile.get();
    header->successiveApproximationHigh = successiveApproximation >> 4;
    header->successiveApproximationLow = successiveApproximation & 0x0F;

    // Baseline JPGs don't use spectral selection or successive approximtion
    if (header->startOfSelection != 0 || header->endOfSelection != 63) {
        std::cout << "Error - Invalid spectral selection\n";
        header->valid = false;
        return;
    }
    if (header->successiveApproximationHigh != 0 || header->successiveApproximationLow != 0) {
        std::cout << "Error - Invalid successive approximation\n";
        header->valid = false;
        return;
    }

    if (length - 6 - (2 * numComponents) != 0) {
        std::cout << "Error - SOS invalid\n";
        header->valid = false;
    }
}

// restart interval is needed to stay synchronized during data scans
void readRestartInterval(std::ifstream& inFile, Header* const header) {
    std::cout << "Reading DRI Marker\n";
    uint length = (inFile.get() << 8) + inFile.get();

    header->restartInterval = (inFile.get() << 8) + inFile.get();
    if (length - 4 != 0) {
        std::cout << "Error - DRI invalid\n";
        header->valid = false;
    }
}

// APPNs simply get skipped based on length
void readAPPN(std::ifstream& inFile, Header* const header) {
    std::cout << "Reading APPN Marker\n";
    uint length = (inFile.get() << 8) + inFile.get();

    for (uint i = 0; i < length - 2; ++i) {
        inFile.get();
    }
}

// comments simply get skipped based on length
void readComment(std::ifstream& inFile, Header* const header) {
    std::cout << "Reading COM Marker\n";
    uint length = (inFile.get() << 8) + inFile.get();

    for (uint i = 0; i < length - 2; ++i) {
        inFile.get();
    }
}

Header* readJPG(const std::string& filename) {
    // open file
    std::ifstream inFile = std::ifstream(filename, std::ios::in | std::ios::binary);
    if (!inFile.is_open()) {
        std::cout << "Error - Error opening input file\n";
        return nullptr;
    }

    Header* header = new (std::nothrow) Header;
    if (header == nullptr) {
        std::cout << "Error - Memory error\n";
        inFile.close();
        return nullptr;
    }

    // first two bytes must be 0xFF, SOI
    byte last = inFile.get();
    byte current = inFile.get();
    if (last != 0xFF || current != SOI) {
        header->valid = false;
        inFile.close();
        return header;
    }
    last = inFile.get();
    current = inFile.get();

    // read markers
    while (header->valid) {
        if (!inFile) {
            std::cout << "Error - File ended prematurely\n";
            header->valid = false;
            inFile.close();
            return header;
        }
        if (last != 0xFF) {
            std::cout << "Error - Expected a marker\n";
            header->valid = false;
            inFile.close();
            return header;
        }

        if (current == SOF0) {
            header->frameType = SOF0;
            readStartOfFrame(inFile, header);
        }
        else if (current == DQT) {
            readQuantizationTable(inFile, header);
        }
        else if (current == DHT) {
            readHuffmanTable(inFile, header);
        }
        else if (current == SOS) {
            readStartOfScan(inFile, header);
            // break from while loop after SOS
            break;
        }
        else if (current == DRI) {
            readRestartInterval(inFile, header);
        }
        else if (current >= APP0 && current <= APP15) {
            readAPPN(inFile, header);
        }
        else if (current == COM) {
            readComment(inFile, header);
        }
        // unused markers that can be skipped
        else if ((current >= JPG0 && current <= JPG13) ||
                current == DNL ||
                current == DHP ||
                current == EXP) {
            readComment(inFile, header);
        }
        else if (current == TEM) {
            // TEM has no size
        }
        // any number of 0xFF in a row is allowed and should be ignored
        else if (current == 0xFF) {
            current = inFile.get();
            continue;
        }

        else if (current == SOI) {
            std::cout << "Error - Embedded JPGs not supported\n";
            header->valid = false;
            inFile.close();
            return header;
        }
        else if (current == EOI) {
            std::cout << "Error - EOI detected before SOS\n";
            header->valid = false;
            inFile.close();
            return header;
        }
        else if (current == DAC) {
            std::cout << "Error - Arithmetic Coding mode not supported\n";
            header->valid = false;
            inFile.close();
            return header;
        }
        else if (current >= SOF0 && current <= SOF15) {
            std::cout << "Error - SOF marker not supported: 0x" << std::hex << (uint)current << std::dec << '\n';
            header->valid = false;
            inFile.close();
            return header;
        }
        else if (current >= RST0 && current <= RST7) {
            std::cout << "Error - RSTN detected before SOS\n";
            header->valid = false;
            inFile.close();
            return header;
        }
        else {
            std::cout << "Error - Unknown marker: 0x" << std::hex << (uint)current << std::dec << '\n';
            header->valid = false;
            inFile.close();
            return header;
        }
        last = inFile.get();
        current = inFile.get();
    }

    // after SOS
    if (header->valid) {
        current = inFile.get();
        // read compressed image data
        while (true) {
            if (!inFile) {
                std::cout << "Error - File ended prematurely\n";
                header->valid = false;
                inFile.close();
                return header;
            }

            last = current;
            current = inFile.get();
            // if marker is found
            if (last == 0xFF) {
                // end of image
                if (current == EOI) {
                    break;
                }
                // 0xFF00 means put a literal 0xFF in image data and ignore 0x00
                else if (current == 0x00) {
                    header->huffmanData.push_back(last);
                    // overwrite 0x00 with next byte
                    current = inFile.get();
                }
                // restart marker
                else if (current >= RST0 && current <= RST7) {
                    // overwrite marker with next byte
                    current = inFile.get();
                }
                // ignore multiple 0xFF's in a row
                else if (current == 0xFF) {
                    // do nothing
                    continue;
                }
                else {
                    std::cout << "Error - Invalid marker during compressed data scan: 0x" << std::hex << (uint)current << std::dec << '\n';
                    header->valid = false;
                    inFile.close();
                    return header;
                }
            }
            else {
                header->huffmanData.push_back(last);
            }
        }
    }

    // validate header info
    if (header->numComponents != 1 && header->numComponents != 3) {
        std::cout << "Error - " << (uint)header->numComponents << " color components given (1 or 3 required)\n";
        header->valid = false;
        inFile.close();
        return header;
    }

    for (uint i = 0; i < header->numComponents; ++i) {
        if (header->quantizationTables[header->colorComponents[i].quantizationTableID].set == false) {
            std::cout << "Error - Color component using uninitialized quantization table\n";
            header->valid = false;
            inFile.close();
            return header;
        }
        if (header->huffmanDCTables[header->colorComponents[i].huffmanDCTableID].set == false) {
            std::cout << "Error - Color component using uninitialized Huffman DC table\n";
            header->valid = false;
            inFile.close();
            return header;
        }
        if (header->huffmanACTables[header->colorComponents[i].huffmanACTableID].set == false) {
            std::cout << "Error - Color component using uninitialized Huffman AC table\n";
            header->valid = false;
            inFile.close();
            return header;
        }
    }

    inFile.close();
    return header;
}

// print all info extracted from the JPG file
void printHeader(const Header* const header) {
    if (header == nullptr) return;
    std::cout << "DQT=============\n";
    for (uint i = 0; i < 4; ++i) {
        if (header->quantizationTables[i].set) {
            std::cout << "Table ID: " << i << '\n';
            std::cout << "Table Data:";
            for (uint j = 0; j < 64; ++j) {
                if (j % 8 == 0) {
                    std::cout << '\n';
                }
                std::cout << header->quantizationTables[i].table[j] << ' ';
            }
            std::cout << '\n';
        }
    }
    std::cout << "SOF=============\n";
    std::cout << "Frame Type: 0x" << std::hex << (uint)header->frameType << std::dec << '\n';
    std::cout << "Height: " << header->height << '\n';
    std::cout << "Width: " << header->width << '\n';
    std::cout << "Color Components:\n";
    for (uint i = 0; i < header->numComponents; ++i) {
        std::cout << "Component ID: " << (i + 1) << '\n';
        std::cout << "Horizontal Sampling Factor: " << (uint)header->colorComponents[i].horizontalSamplingFactor << '\n';
        std::cout << "Vertical Sampling Factor: " << (uint)header->colorComponents[i].verticalSamplingFactor << '\n';
        std::cout << "Quantization Table ID: " << (uint)header->colorComponents[i].quantizationTableID << '\n';
    }
    std::cout << "DHT=============\n";
    std::cout << "DC Tables:\n";
    for (uint i = 0; i < 4; ++i) {
        if (header->huffmanDCTables[i].set) {
            std::cout << "Table ID: " << i << '\n';
            std::cout << "Symbols:\n";
            for (uint j = 0; j < 16; ++j) {
                std::cout << (j + 1) << ": ";
                for (uint k = header->huffmanDCTables[i].offsets[j]; k < header->huffmanDCTables[i].offsets[j + 1]; ++k) {
                    std::cout << std::hex << (uint)header->huffmanDCTables[i].symbols[k] << std::dec << ' ';
                }
                std::cout << '\n';
            }
        }
    }
    std::cout << "AC Tables:\n";
    for (uint i = 0; i < 4; ++i) {
        if (header->huffmanACTables[i].set) {
            std::cout << "Table ID: " << i << '\n';
            std::cout << "Symbols:\n";
            for (uint j = 0; j < 16; ++j) {
                std::cout << (j + 1) << ": ";
                for (uint k = header->huffmanACTables[i].offsets[j]; k < header->huffmanACTables[i].offsets[j + 1]; ++k) {
                    std::cout << std::hex << (uint)header->huffmanACTables[i].symbols[k] << std::dec << ' ';
                }
                std::cout << '\n';
            }
        }
    }
    std::cout << "SOS=============\n";
    std::cout << "Start of Selection: " << (uint)header->startOfSelection << '\n';
    std::cout << "End of Selection: " << (uint)header->endOfSelection << '\n';
    std::cout << "Successive Approximation High: " << (uint)header->successiveApproximationHigh << '\n';
    std::cout << "Successive Approximation Low: " << (uint)header->successiveApproximationLow << '\n';
    std::cout << "Color Components:\n";
    for (uint i = 0; i < header->numComponents; ++i) {
        std::cout << "Component ID: " << (i + 1) << '\n';
        std::cout << "Huffman DC Table ID: " << (uint)header->colorComponents[i].huffmanDCTableID << '\n';
        std::cout << "Huffman AC Table ID: " << (uint)header->colorComponents[i].huffmanACTableID << '\n';
    }
    std::cout << "Length of Huffman Data: " << header->huffmanData.size() << '\n';
    std::cout << "DRI=============\n";
    std::cout << "Restart Interval: " << header->restartInterval << '\n';
}

// generate all Huffman codes based on symbols from a Huffman table
void generateCodes(HuffmanTable& hTable) {
    uint code = 0;
    for (uint i = 0; i < 16; ++i) {
        for (uint j = hTable.offsets[i]; j < hTable.offsets[i + 1]; ++j) {
            hTable.codes[j] = code;
            code += 1;
        }
        code <<= 1;
    }
}

// helper class to read bits from a byte vector
class BitReader {
private:
    uint nextByte = 0;
    uint nextBit = 0;
    const std::vector<byte>& data;

public:
    BitReader(const std::vector<byte>& d) :
    data(d)
    {}

    // read one bit (0 or 1) or return -1 if all bits have already been read
    int readBit() {
        if (nextByte >= data.size()) {
            return -1;
        }
        int bit = (data[nextByte] >> (7 - nextBit)) & 1;
        nextBit += 1;
        if (nextBit == 8) {
            nextBit = 0;
            nextByte += 1;
        }
        return bit;
    }

    // read a variable number of bits
    // first read bit is most significant bit
    // return -1 if at any point all bits have already been read
    int readBits(const uint length) {
        int bits = 0;
        for (uint i = 0; i < length; ++i) {
            int bit = readBit();
            if (bit == -1) {
                bits = -1;
                break;
            }
            bits = (bits << 1) | bit;
        }
        return bits;
    }

    // if there are bits remaining,
    //   advance to the 0th bit of the next byte
    void align() {
        if (nextByte >= data.size()) {
            return;
        }
        if (nextBit != 0) {
            nextBit = 0;
            nextByte += 1;
        }
    }
};

// return the symbol from the Huffman table that corresponds to
//   the next Huffman code read from the BitReader
byte getNextSymbol(BitReader& b, const HuffmanTable& hTable) {
    uint currentCode = 0;
    for (uint i = 0; i < 16; ++i) {
        int bit = b.readBit();
        if (bit == -1) {
            return -1;
        }
        currentCode = (currentCode << 1) | bit;
        for (uint j = hTable.offsets[i]; j < hTable.offsets[i + 1]; ++j) {
            if (currentCode == hTable.codes[j]) {
                return hTable.symbols[j];
            }
        }
    }
    return -1;
}

// fill the coefficients of an MCU component based on Huffman codes
//   read from the BitReader
bool decodeMCUComponent(BitReader& b, int* const component, int& previousDC,
                        const HuffmanTable& dcTable,
                        const HuffmanTable& acTable) {
    // get the DC value for this MCU component
    byte length = getNextSymbol(b, dcTable);
    if (length == (byte)-1) {
        std::cout << "Error - Invalid DC value\n";
        return false;
    }
    if (length > 11) {
        std::cout << "Error - DC coefficient length greater than 11\n";
        return false;
    }

    int coeff = b.readBits(length);
    if (coeff == -1) {
        std::cout << "Error - Invalid DC value\n";
        return false;
    }
    if (length != 0 && coeff < (1 << (length - 1))) {
        coeff -= (1 << length) - 1;
    }
    component[0] = coeff + previousDC;
    previousDC = component[0];

    // get the AC values for this MCU component
    uint i = 1;
    while (i < 64) {
        byte symbol = getNextSymbol(b, acTable);
        if (symbol == (byte)-1) {
            std::cout << "Error - Invalid AC value\n";
            return false;
        }

        // symbol 0x00 means fill remainder of component with 0
        if (symbol == 0x00) {
            for (; i < 64; ++i) {
                component[zigZagMap[i]] = 0;
            }
            return true;
        }

        // otherwise, read next component coefficient
        byte numZeroes = symbol >> 4;
        byte coeffLength = symbol & 0x0F;
        coeff = 0;

        // symbol 0xF0 means skip 16 0's
        if (symbol == 0xF0) {
            numZeroes = 16;
        }

        if (i + numZeroes >= 64) {
            std::cout << "Error - Zero run-length exceeded MCU\n";
            return false;
        }
        for (uint j = 0; j < numZeroes; ++j, ++i) {
            component[zigZagMap[i]] = 0;
        }

        if (coeffLength > 10) {
            std::cout << "Error - AC coefficient length greater than 10\n";
            return false;
        }
        if (coeffLength != 0) {
            coeff = b.readBits(coeffLength);
            if (coeff == -1) {
                std::cout << "Error - Invalid AC value\n";
                return false;
            }
            if (coeff < (1 << (coeffLength - 1))) {
                coeff -= (1 << coeffLength) - 1;
            }
            component[zigZagMap[i]] = coeff;
            i += 1;
        }
    }
    return true;
}

// decode all the Huffman data and fill all MCUs
MCU* decodeHuffmanData(Header* const header) {
    MCU* mcus = new (std::nothrow) MCU[header->mcuHeightReal * header->mcuWidthReal];
    if (mcus == nullptr) {
        std::cout << "Error - Memory error\n";
        return nullptr;
    }

    for (uint i = 0; i < 4; ++i) {
        if (header->huffmanDCTables[i].set) {
            generateCodes(header->huffmanDCTables[i]);
        }
        if (header->huffmanACTables[i].set) {
            generateCodes(header->huffmanACTables[i]);
        }
    }

    BitReader b(header->huffmanData);

    int previousDCs[3] = { 0 };
    uint restartInterval = header->restartInterval * header->horizontalSamplingFactor * header->verticalSamplingFactor;

    for (uint y = 0; y < header->mcuHeight; y += header->verticalSamplingFactor) {
        for (uint x = 0; x < header->mcuWidth; x += header->horizontalSamplingFactor) {
            if (restartInterval != 0 && (y * header->mcuWidthReal + x) % restartInterval == 0) {
                previousDCs[0] = 0;
                previousDCs[1] = 0;
                previousDCs[2] = 0;
                b.align();
            }

            for (uint i = 0; i < header->numComponents; ++i) {
                for (uint v = 0; v < header->colorComponents[i].verticalSamplingFactor; ++v) {
                    for (uint h = 0; h < header->colorComponents[i].horizontalSamplingFactor; ++h) {
                        if (!decodeMCUComponent(
                                b,
                                mcus[(y + v) * header->mcuWidthReal + (x + h)][i],
                                previousDCs[i],
                                header->huffmanDCTables[header->colorComponents[i].huffmanDCTableID],
                                header->huffmanACTables[header->colorComponents[i].huffmanACTableID])) {
                            delete[] mcus;
                            return nullptr;
                        }
                    }
                }
            }
        }
    }
    return mcus;
}

// dequantize an MCU component based on a quantization table
void dequantizeMCUComponent(const QuantizationTable& qTable, int* const component) {
    for (uint i = 0; i < 64; ++i) {
        component[i] *= qTable.table[i];
    }
}

// dequantize all MCUs
void dequantize(const Header* const header, MCU* const mcus) {
    for (uint y = 0; y < header->mcuHeight; y += header->verticalSamplingFactor) {
        for (uint x = 0; x < header->mcuWidth; x += header->horizontalSamplingFactor) {
            for (uint i = 0; i < header->numComponents; ++i) {
                for (uint v = 0; v < header->colorComponents[i].verticalSamplingFactor; ++v) {
                    for (uint h = 0; h < header->colorComponents[i].horizontalSamplingFactor; ++h) {
                        dequantizeMCUComponent(header->quantizationTables[header->colorComponents[i].quantizationTableID], mcus[(y + v) * header->mcuWidthReal + (x + h)][i]);
                    }
                }
            }
        }
    }
}

// perform 1-D IDCT on all columns and rows of an MCU component
//   resulting in 2-D IDCT
void inverseDCTComponent(int* const component) {
    for (uint i = 0; i < 8; ++i) {
        const float g0 = component[0 * 8 + i] * s0;
        const float g1 = component[4 * 8 + i] * s4;
        const float g2 = component[2 * 8 + i] * s2;
        const float g3 = component[6 * 8 + i] * s6;
        const float g4 = component[5 * 8 + i] * s5;
        const float g5 = component[1 * 8 + i] * s1;
        const float g6 = component[7 * 8 + i] * s7;
        const float g7 = component[3 * 8 + i] * s3;

        const float f0 = g0;
        const float f1 = g1;
        const float f2 = g2;
        const float f3 = g3;
        const float f4 = g4 - g7;
        const float f5 = g5 + g6;
        const float f6 = g5 - g6;
        const float f7 = g4 + g7;

        const float e0 = f0;
        const float e1 = f1;
        const float e2 = f2 - f3;
        const float e3 = f2 + f3;
        const float e4 = f4;
        const float e5 = f5 - f7;
        const float e6 = f6;
        const float e7 = f5 + f7;
        const float e8 = f4 + f6;

        const float d0 = e0;
        const float d1 = e1;
        const float d2 = e2 * m1;
        const float d3 = e3;
        const float d4 = e4 * m2;
        const float d5 = e5 * m3;
        const float d6 = e6 * m4;
        const float d7 = e7;
        const float d8 = e8 * m5;

        const float c0 = d0 + d1;
        const float c1 = d0 - d1;
        const float c2 = d2 - d3;
        const float c3 = d3;
        const float c4 = d4 + d8;
        const float c5 = d5 + d7;
        const float c6 = d6 - d8;
        const float c7 = d7;
        const float c8 = c5 - c6;

        const float b0 = c0 + c3;
        const float b1 = c1 + c2;
        const float b2 = c1 - c2;
        const float b3 = c0 - c3;
        const float b4 = c4 - c8;
        const float b5 = c8;
        const float b6 = c6 - c7;
        const float b7 = c7;

        component[0 * 8 + i] = b0 + b7;
        component[1 * 8 + i] = b1 + b6;
        component[2 * 8 + i] = b2 + b5;
        component[3 * 8 + i] = b3 + b4;
        component[4 * 8 + i] = b3 - b4;
        component[5 * 8 + i] = b2 - b5;
        component[6 * 8 + i] = b1 - b6;
        component[7 * 8 + i] = b0 - b7;
    }
    for (uint i = 0; i < 8; ++i) {
        const float g0 = component[i * 8 + 0] * s0;
        const float g1 = component[i * 8 + 4] * s4;
        const float g2 = component[i * 8 + 2] * s2;
        const float g3 = component[i * 8 + 6] * s6;
        const float g4 = component[i * 8 + 5] * s5;
        const float g5 = component[i * 8 + 1] * s1;
        const float g6 = component[i * 8 + 7] * s7;
        const float g7 = component[i * 8 + 3] * s3;

        const float f0 = g0;
        const float f1 = g1;
        const float f2 = g2;
        const float f3 = g3;
        const float f4 = g4 - g7;
        const float f5 = g5 + g6;
        const float f6 = g5 - g6;
        const float f7 = g4 + g7;

        const float e0 = f0;
        const float e1 = f1;
        const float e2 = f2 - f3;
        const float e3 = f2 + f3;
        const float e4 = f4;
        const float e5 = f5 - f7;
        const float e6 = f6;
        const float e7 = f5 + f7;
        const float e8 = f4 + f6;

        const float d0 = e0;
        const float d1 = e1;
        const float d2 = e2 * m1;
        const float d3 = e3;
        const float d4 = e4 * m2;
        const float d5 = e5 * m3;
        const float d6 = e6 * m4;
        const float d7 = e7;
        const float d8 = e8 * m5;

        const float c0 = d0 + d1;
        const float c1 = d0 - d1;
        const float c2 = d2 - d3;
        const float c3 = d3;
        const float c4 = d4 + d8;
        const float c5 = d5 + d7;
        const float c6 = d6 - d8;
        const float c7 = d7;
        const float c8 = c5 - c6;

        const float b0 = c0 + c3;
        const float b1 = c1 + c2;
        const float b2 = c1 - c2;
        const float b3 = c0 - c3;
        const float b4 = c4 - c8;
        const float b5 = c8;
        const float b6 = c6 - c7;
        const float b7 = c7;

        component[i * 8 + 0] = b0 + b7;
        component[i * 8 + 1] = b1 + b6;
        component[i * 8 + 2] = b2 + b5;
        component[i * 8 + 3] = b3 + b4;
        component[i * 8 + 4] = b3 - b4;
        component[i * 8 + 5] = b2 - b5;
        component[i * 8 + 6] = b1 - b6;
        component[i * 8 + 7] = b0 - b7;
    }
}

// perform IDCT on all MCUs
void inverseDCT(const Header* const header, MCU* const mcus) {
    for (uint y = 0; y < header->mcuHeight; y += header->verticalSamplingFactor) {
        for (uint x = 0; x < header->mcuWidth; x += header->horizontalSamplingFactor) {
            for (uint i = 0; i < header->numComponents; ++i) {
                for (uint v = 0; v < header->colorComponents[i].verticalSamplingFactor; ++v) {
                    for (uint h = 0; h < header->colorComponents[i].horizontalSamplingFactor; ++h) {
                        inverseDCTComponent(mcus[(y + v) * header->mcuWidthReal + (x + h)][i]);
                    }
                }
            }
        }
    }
}

// convert all pixels in an MCU from YCbCr color space to RGB
void YCbCrToRGBMCU(const Header* const header, MCU& mcu, const MCU& cbcr, const uint v, const uint h) {
    for (uint y = 7; y < 8; --y) {
        for (uint x = 7; x < 8; --x) {
            const uint pixel = y * 8 + x;
            const uint cbcrPixelRow = y / header->verticalSamplingFactor + 4 * v;
            const uint cbcrPixelColumn = x / header->horizontalSamplingFactor + 4 * h;
            const uint cbcrPixel = cbcrPixelRow * 8 + cbcrPixelColumn;
            int r = mcu.y[pixel]                               + 1.402f * cbcr.cr[cbcrPixel] + 128;
            int g = mcu.y[pixel] - 0.344f * cbcr.cb[cbcrPixel] - 0.714f * cbcr.cr[cbcrPixel] + 128;
            int b = mcu.y[pixel] + 1.772f * cbcr.cb[cbcrPixel]                               + 128;
            if (r < 0)   r = 0;
            if (r > 255) r = 255;
            if (g < 0)   g = 0;
            if (g > 255) g = 255;
            if (b < 0)   b = 0;
            if (b > 255) b = 255;
            mcu.r[pixel] = r;
            mcu.g[pixel] = g;
            mcu.b[pixel] = b;
        }
    }
}

// convert all pixels from YCbCr color space to RGB
void YCbCrToRGB(const Header* const header, MCU* const mcus) {
    for (uint y = 0; y < header->mcuHeight; y += header->verticalSamplingFactor) {
        for (uint x = 0; x < header->mcuWidth; x += header->horizontalSamplingFactor) {
            const MCU& cbcr = mcus[y * header->mcuWidthReal + x];
            for (uint v = header->verticalSamplingFactor - 1; v < header->verticalSamplingFactor; --v) {
                for (uint h = header->horizontalSamplingFactor - 1; h < header->horizontalSamplingFactor; --h) {
                    MCU& mcu = mcus[(y + v) * header->mcuWidthReal + (x + h)];
                    YCbCrToRGBMCU(header, mcu, cbcr, v, h);
                }
            }
        }
    }
}

// helper function to write a 4-byte integer in little-endian
void putInt(std::ofstream& outFile, const uint v) {
    outFile.put((v >>  0) & 0xFF);
    outFile.put((v >>  8) & 0xFF);
    outFile.put((v >> 16) & 0xFF);
    outFile.put((v >> 24) & 0xFF);
}

// helper function to write a 2-byte short integer in little-endian
void putShort(std::ofstream& outFile, const uint v) {
    outFile.put((v >>  0) & 0xFF);
    outFile.put((v >>  8) & 0xFF);
}

// write all the pixels in the MCUs to a BMP file
void writeBMP(const Header* const header, const MCU* const mcus, const std::string& filename) {
    // open file
    std::ofstream outFile = std::ofstream(filename, std::ios::out | std::ios::binary);
    if (!outFile.is_open()) {
        std::cout << "Error - Error opening output file\n";
        return;
    }

    const uint paddingSize = header->width % 4;
    const uint size = 14 + 12 + header->height * header->width * 3 + paddingSize * header->height;

    outFile.put('B');
    outFile.put('M');
    putInt(outFile, size);
    putInt(outFile, 0);
    putInt(outFile, 0x1A);
    putInt(outFile, 12);
    putShort(outFile, header->width);
    putShort(outFile, header->height);
    putShort(outFile, 1);
    putShort(outFile, 24);

    for (uint y = header->height - 1; y < header->height; --y) {
        const uint mcuRow = y / 8;
        const uint pixelRow = y % 8;
        for (uint x = 0; x < header->width; ++x) {
            const uint mcuColumn = x / 8;
            const uint pixelColumn = x % 8;
            const uint mcuIndex = mcuRow * header->mcuWidthReal + mcuColumn;
            const uint pixelIndex = pixelRow * 8 + pixelColumn;
            outFile.put(mcus[mcuIndex].b[pixelIndex]);
            outFile.put(mcus[mcuIndex].g[pixelIndex]);
            outFile.put(mcus[mcuIndex].r[pixelIndex]);
        }
        for (uint i = 0; i < paddingSize; ++i) {
            outFile.put(0);
        }
    }

    outFile.close();
}

int main(int argc, char** argv) {
    // validate arguments
    if (argc < 2) {
        std::cout << "Error - Invalid arguments\n";
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string filename(argv[i]);

        // read header
        Header* header = readJPG(filename);
        // validate header
        if (header == nullptr) {
            continue;
        }
        if (header->valid == false) {
            std::cout << "Error - Invalid JPG\n";
            delete header;
            continue;
        }

        printHeader(header);

        // decode Huffman data
        MCU* mcus = decodeHuffmanData(header);
        if (mcus == nullptr) {
            delete header;
            continue;
        }

        // dequantize MCU coefficients
        dequantize(header, mcus);

        // Inverse Discrete Cosine Transform
        inverseDCT(header, mcus);

        // color conversion
        YCbCrToRGB(header, mcus);

        // write BMP file
        const std::size_t pos = filename.find_last_of('.');
        const std::string outFilename = (pos == std::string::npos) ? (filename + ".bmp") : (filename.substr(0, pos) + ".bmp");
        writeBMP(header, mcus, outFilename);

        delete[] mcus;
        delete header;
    }
    return 0;
}
