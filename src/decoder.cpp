#include <iostream>
#include <fstream>
#include <string>

#include "jpg.h"

// SOF specifies frame type, dimensions, and number of color components
void readStartOfFrame(std::ifstream& inFile, Header* header) {
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
void readQuantizationTable(std::ifstream& inFile, Header* header) {
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
                header->quantizationTables[tableID].table[i] = (inFile.get() << 8) + inFile.get();
            }
            length -= 128;
        }
        else {
            for (uint i = 0; i < 64; ++i) {
                header->quantizationTables[tableID].table[i] = inFile.get();
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
void readHuffmanTable(std::ifstream& inFile, Header* header) {
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
void readStartOfScan(std::ifstream& inFile, Header* header) {
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
void readRestartInterval(std::ifstream& inFile, Header* header) {
    uint length = (inFile.get() << 8) + inFile.get();

    header->restartInterval = (inFile.get() << 8) + inFile.get();
    if (length - 4 != 0) {
        std::cout << "Error - DRI invalid\n";
        header->valid = false;
    }
}

// APPNs simply get skipped based on length
void readAPPN(std::ifstream& inFile, Header* header) {
    uint length = (inFile.get() << 8) + inFile.get();

    for (uint i = 0; i < length - 2; ++i) {
        inFile.get();
    }
}

// comments simply get skipped based on length
void readComment(std::ifstream& inFile, Header* header) {
    uint length = (inFile.get() << 8) + inFile.get();

    for (uint i = 0; i < length - 2; ++i) {
        inFile.get();
    }
}

Header* readJPG(std::ifstream& inFile) {
    Header* header = new (std::nothrow) Header;
    byte last, current;

    if (header == nullptr) {
        return header;
    }

    // first two bytes must be 0xFF, SOI
    last = inFile.get();
    current = inFile.get();
    if (last != 0xFF || current != SOI) {
        header->valid = false;
        return header;
    }
    last = inFile.get();
    current = inFile.get();

    // read markers
    while (header->valid) {
        if (!inFile) {
            std::cout << "Error - File ended prematurely\n";
            header->valid = false;
            return header;
        }
        if (last != 0xFF) {
            std::cout << "Error - Expected a marker\n";
            header->valid = false;
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
            return header;
        }
        else if (current == EOI) {
            std::cout << "Error - EOI detected before SOS\n";
            header->valid = false;
            return header;
        }
        else if (current == DAC) {
            std::cout << "Error - Arithmetic Coding mode not supported\n";
            header->valid = false;
            return header;
        }
        else if (current >= SOF0 && current <= SOF15) {
            std::cout << "Error - SOF marker not supported: 0x" << std::hex << (uint)current << std::dec << '\n';
            header->valid = false;
            return header;
        }
        else if (current >= RST0 && current <= RST7) {
            std::cout << "Error - RSTN detected before SOS\n";
            header->valid = false;
            return header;
        }
        else {
            std::cout << "Error - Unknown marker: 0x" << std::hex << (uint)current << std::dec << '\n';
            header->valid = false;
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
        return header;
    }

    if (header->colorComponents[0].horizontalSamplingFactor != 1 ||
        header->colorComponents[0].verticalSamplingFactor != 1) {
        std::cout << "Error - Unsupported sampling factor\n";
        header->valid = false;
        return header;
    }
    if (header->numComponents == 3) {
        if (header->colorComponents[1].horizontalSamplingFactor != 1 ||
            header->colorComponents[1].verticalSamplingFactor != 1) {
            std::cout << "Error - Unsupported sampling factor\n";
            header->valid = false;
            return header;
        }
        if (header->colorComponents[2].horizontalSamplingFactor != 1 ||
            header->colorComponents[2].verticalSamplingFactor != 1) {
            std::cout << "Error - Unsupported sampling factor\n";
            header->valid = false;
            return header;
        }
    }
    for (uint i = 0; i < header->numComponents; ++i) {
        if (header->quantizationTables[header->colorComponents[i].quantizationTableID].set == false) {
            std::cout << "Error - Color component using uninitialized quantization table\n";
            header->valid = false;
            return header;
        }
        if (header->huffmanDCTables[header->colorComponents[i].huffmanDCTableID].set == false) {
            std::cout << "Error - Color component using uninitialized Huffman DC table\n";
            header->valid = false;
            return header;
        }
        if (header->huffmanACTables[header->colorComponents[i].huffmanACTableID].set == false) {
            std::cout << "Error - Color component using uninitialized Huffman AC table\n";
            header->valid = false;
            return header;
        }
    }

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
    std::cout << "DHT=============\n";
    std::cout << "DC Tables:\n";
    for (uint i = 0; i < 4; ++i) {
        if (header->huffmanDCTables[i].set) {
            std::cout << "Table ID: " << i << '\n';
            std::cout << "Symbols:\n";
            for (uint j = 0; j < 16; ++j) {
                std::cout << (j + 1) << ": ";
                for (uint k = header->huffmanDCTables[i].offsets[j]; k < header->huffmanDCTables[i].offsets[j + 1]; ++k) {
                    std::cout << (uint)header->huffmanDCTables[i].symbols[k] << ' ';
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
                    std::cout << (uint)header->huffmanACTables[i].symbols[k] << ' ';
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
    std::cout << "Restart Interval: " << (uint)header->restartInterval << '\n';
    std::cout << "Color Components:\n";
    for (uint i = 0; i < header->numComponents; ++i) {
        std::cout << "Component ID: " << (i + 1) << '\n';
        std::cout << "Horizontal Sampling Factor: " << (uint)header->colorComponents[i].horizontalSamplingFactor << '\n';
        std::cout << "Vertical Sampling Factor: " << (uint)header->colorComponents[i].verticalSamplingFactor << '\n';
        std::cout << "Quantization Table ID: " << (uint)header->colorComponents[i].quantizationTableID << '\n';
        std::cout << "Huffman DC Table ID: " << (uint)header->colorComponents[i].huffmanDCTableID << '\n';
        std::cout << "Huffman AC Table ID: " << (uint)header->colorComponents[i].huffmanACTableID << '\n';
    }
    std::cout << "Length of Huffman Data: " << header->huffmanData.size() << '\n';
}

void generateCodes(const HuffmanTable& hTable, int* codes) {
    int code = 0;
    for (uint j = 0; j < 16; ++j) {
        for (uint k = hTable.offsets[j]; k < hTable.offsets[j + 1]; ++k) {
            codes[k] = code;
            code += 1;
        }
        code <<= 1;
    }
}

class BitReader {
private:
    uint nextByte = 0;
    uint nextBit = 0;
    const std::vector<byte>& data;

public:
    BitReader(const std::vector<byte>& d) :
    data(d)
    {}

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

byte getNextSymbol(BitReader& b, const int* const codes, const HuffmanTable& hTable) {
    int currentCode = b.readBit();
    for (uint j = 0; j < 16; ++j) {
        for (uint k = hTable.offsets[j]; k < hTable.offsets[j + 1]; ++k) {
            if (currentCode == codes[k]) {
                return hTable.symbols[k];
            }
        }
        currentCode = (currentCode << 1) | b.readBit();
    }
    return -1;
}

bool decodeMCUComponent(BitReader& b, int* component, const int previousDC,
                        const int* const dcCodes, const HuffmanTable& dcTable,
                        const int* const acCodes, const HuffmanTable& acTable) {
    // get the DC value for this MCU component
    byte length = getNextSymbol(b, dcCodes, dcTable);
    if (length == (byte)-1) {
        std::cout << "Error - Invalid DC value\n";
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

    // get the AC values for this MCU component
    for (uint i = 1; i < 64; ++i) {
        byte symbol = getNextSymbol(b, acCodes, acTable);
        if (symbol == -1) {
            std::cout << "Error - Invalid AC value\n";
            return false;
        }

        // symbol 0 means fill remainder of component with 0
        if (symbol == 0) {
            for (; i < 64; ++i) {
                component[zigZagMap[i]] = 0;
            }
            return true;
        }

        // otherwise, read next component coefficient
        byte numZeroes = symbol >> 4;
        byte coeffLength = symbol & 0x0F;
        coeff = 0;

        for (uint j = 0; j < numZeroes && i < 64; ++j, ++i) {
            component[zigZagMap[i]] = 0;
        }

        if (coeffLength > 10) {
            std::cout << "Error - Coefficient length greater than 10\n";
            return false;
        }

        if (coeffLength != 0) {
            if (i == 64) {
                std::cout << "Error - Zero run-length exceeded MCU\n";
                return false;
            }

            coeff = b.readBits(coeffLength);
            if (coeff == -1) {
                std::cout << "Error - Invalid AC value\n";
                return false;
            }
            if (coeff < (1 << (coeffLength - 1))) {
                coeff -= (1 << coeffLength) - 1;
            }
            component[zigZagMap[i]] = coeff;
        }
    }
    return true;
}

bool decodeHuffmanData(const Header* const header, MCU* mcus) {
    BitReader b(header->huffmanData);

    int dcCodes[4][162];
    int acCodes[4][162];
    for (uint i = 0; i < 4; ++i) {
        if (header->huffmanDCTables[i].set) {
            generateCodes(header->huffmanDCTables[i], dcCodes[i]);
        }
        if (header->huffmanACTables[i].set) {
            generateCodes(header->huffmanACTables[i], acCodes[i]);
        }
    }

    const byte yDCTableID  = header->colorComponents[0].huffmanDCTableID;
    const byte yACTableID  = header->colorComponents[0].huffmanACTableID;
    const byte cbDCTableID = header->colorComponents[1].huffmanDCTableID;
    const byte cbACTableID = header->colorComponents[1].huffmanACTableID;
    const byte crDCTableID = header->colorComponents[2].huffmanDCTableID;
    const byte crACTableID = header->colorComponents[2].huffmanACTableID;
    int previousYDC  = 0;
    int previousCbDC = 0;
    int previousCrDC = 0;

    const uint mcuHeight = (header->height + 7) / 8;
    const uint mcuWidth = (header->width + 7) / 8;
    uint numProcessed = 0;
    for (uint i = 0; i < mcuHeight * mcuWidth; ++i) {
        if (!decodeMCUComponent(b, mcus[i].y, previousYDC,
                                dcCodes[yDCTableID], header->huffmanDCTables[yDCTableID],
                                acCodes[yACTableID], header->huffmanACTables[yACTableID])) {
            return false;
        }
        previousYDC = mcus[i].y[0];
        if (header->numComponents == 3) {
            if (!decodeMCUComponent(b, mcus[i].cb, previousCbDC,
                                    dcCodes[cbDCTableID], header->huffmanDCTables[cbDCTableID],
                                    acCodes[cbACTableID], header->huffmanACTables[cbACTableID])) {
                return false;
            }
            previousCbDC = mcus[i].cb[0];
            if (!decodeMCUComponent(b, mcus[i].cr, previousCrDC,
                                    dcCodes[crDCTableID], header->huffmanDCTables[crDCTableID],
                                    acCodes[crACTableID], header->huffmanACTables[crACTableID])) {
                return false;
            }
            previousCrDC = mcus[i].cr[0];
        }

        numProcessed += 1;
        if (header->restartInterval != 0 && numProcessed % header->restartInterval == 0) {
            previousYDC  = 0;
            previousCbDC = 0;
            previousCrDC = 0;
            b.align();
        }
    }
    return true;
}

int main(int argc, char** argv) {
    // validate arguments
    if (argc != 2) {
        std::cout << "Error - Invalid arguments\n";
        return 1;
    }

    // open file
    std::string filename = std::string(argv[1]);
    std::ifstream inFile = std::ifstream(filename, std::ios::in | std::ios::binary);
    if (!inFile.is_open()) {
        std::cout << "Error - Error opening input file\n";
        return 1;
    }

    // read header
    Header* header = readJPG(inFile);
    inFile.close();

    // validate header
    if (header == nullptr) {
        std::cout << "Error - Memory error\n";
        return 1;
    }
    if (header->valid == false) {
        std::cout << "Error - Invalid JPG\n";
        delete header;
        return 1;
    }

    printHeader(header);

    // decode Huffman data
    const uint mcuHeight = (header->height + 7) / 8;
    const uint mcuWidth = (header->width + 7) / 8;
    MCU* mcus = new (std::nothrow) MCU[mcuHeight * mcuWidth];
    if (mcus == nullptr) {
        std::cout << "Error - Memory error\n";
        delete header;
        return 1;
    }

    if (!decodeHuffmanData(header, mcus)) {
        delete[] mcus;
        delete header;
        return 1;
    }

    delete[] mcus;
    delete header;
    return 0;
}
