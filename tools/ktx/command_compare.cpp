// Copyright 2023-2023 The Khronos Group Inc.
// Copyright 2023-2023 RasterGrid Kft.
// SPDX-License-Identifier: Apache-2.0

#include "command.h"
#include "sbufstream.h"
#include "stdafx.h"
#include "utility.h"
#include "validate.h"
#include "formats.h"
#include "basis_sgd.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>
#include <iomanip>
#include <map>

#include <cxxopts.hpp>
#include <fmt/printf.h>
#include <ktx.h>
#include <ktxint.h>


// -------------------------------------------------------------------------------------------------

namespace ktx {

extern "C" {
    const char* ktxBUImageFlagsBitString(ktx_uint32_t bit_index, bool bit_value);
}

template <typename T>
struct DiffBase {
    DiffBase(const std::string_view textHeaderIn, const std::string_view jsonPathIn,
        const std::optional<T>& value1, const std::optional<T>& value2)
        : textHeader(textHeaderIn), jsonPath(jsonPathIn), values(), different(value1 != value2) {
        values[0] = value1;
        values[1] = value2;
    }

    DiffBase(const std::string_view textHeaderIn, const std::string_view jsonPathIn,
        const T& value1, const T& value2)
        : textHeader(textHeaderIn), jsonPath(jsonPathIn), values(), different(value1 != value2) {
        values[0] = value1;
        values[1] = value2;
    }

    const std::string_view textHeader;
    const std::string_view jsonPath;

    bool isDifferent() const {
        return different;
    }

    bool hasValue(std::size_t index) const {
        return values[index].has_value();
    }

    virtual std::string value(std::size_t index, OutputFormat format) const = 0;

protected:
    const T& rawValue(std::size_t index) const {
        return *values[index];
    }

private:
    std::optional<T> values[2];
    const bool different;
};

template <typename T>
struct Diff : public DiffBase<T> {
    Diff(const std::string_view textHeaderIn, const std::string_view jsonPathIn,
        const std::optional<T>& value1, const std::optional<T>& value2)
        : DiffBase<T>(textHeaderIn, jsonPathIn, value1, value2) {}

    Diff(const std::string_view textHeaderIn, const std::string_view jsonPathIn,
        const T& value1, const T& value2)
        : DiffBase<T>(textHeaderIn, jsonPathIn, value1, value2) {}

    virtual std::string value(std::size_t index, OutputFormat) const override {
        return fmt::format("{}", DiffBase<T>::rawValue(index));
    }
};

struct DiffIdentifier : public DiffBase<std::array<uint8_t, sizeof(KTX_header2::identifier)>> {
    DiffIdentifier(const std::string_view textHeaderIn, const std::string_view jsonPathIn,
        const KTX_header2& value1, const KTX_header2& value2)
        : DiffBase<std::array<uint8_t, sizeof(KTX_header2::identifier)>>(textHeaderIn, jsonPathIn,
            to_array(value1), to_array(value2)) {
    }

    virtual std::string value(std::size_t index, OutputFormat format) const override {
        // Convert identifier for better display.
        auto identifier = rawValue(index);
        uint32_t idlen = 0;
        char u8identifier[30];
        for (uint32_t i = 0; i < 12 && idlen < sizeof(u8identifier); i++, idlen++) {
            // Convert the angle brackets to utf-8 for better printing. The
            // conversion below only works for characters whose msb's are 10.
            if (identifier[i] == U'\xAB') {
                u8identifier[idlen++] = '\xc2';
                u8identifier[idlen] = identifier[i];
            } else if (identifier[i] == U'\xBB') {
                u8identifier[idlen++] = '\xc2';
                u8identifier[idlen] = identifier[i];
            } else if (identifier[i] < '\x20') {
                uint32_t nchars;
                switch (identifier[i]) {
                case '\n':
                    u8identifier[idlen++] = '\\';
                    u8identifier[idlen] = 'n';
                    break;
                case '\r':
                    u8identifier[idlen++] = '\\';
                    u8identifier[idlen] = 'r';
                    break;
                default:
                    nchars = snprintf(&u8identifier[idlen],
                                    sizeof(u8identifier) - idlen,
                                    format == OutputFormat::text ? "\\x%02X" : "\\u%04X",
                                    identifier[i]);
                    idlen += nchars - 1;
                }
            } else {
                u8identifier[idlen] = identifier[i];
            }
        }
        if (format == OutputFormat::text) {
            return std::string(u8identifier, idlen);
        } else {
            return fmt::format("\"{}\"", std::string(u8identifier, idlen));
        }
    }

private:
    static std::array<uint8_t, sizeof(KTX_header2::identifier)> to_array(const KTX_header2& header) {
        std::array<uint8_t, sizeof(KTX_header2::identifier)> arr{};
        for (std::size_t i = 0; i < sizeof(KTX_header2::identifier); ++i) {
            arr[i] = header.identifier[i];
        }
        return arr;
    }
};

template <typename T>
struct DiffHex : public DiffBase<T> {
    DiffHex(const std::string_view textHeaderIn, const std::string_view jsonPathIn,
        const std::optional<T>& value1, const std::optional<T>& value2)
        : DiffBase<T>(textHeaderIn, jsonPathIn, value1, value2) {}

    DiffHex(const std::string_view textHeaderIn, const std::string_view jsonPathIn,
        const T& value1, const T& value2)
        : DiffBase<T>(textHeaderIn, jsonPathIn, value1, value2) {}

    virtual std::string value(std::size_t index, OutputFormat format) const override {
        if (format == OutputFormat::text) {
            return fmt::format("0x{:x}", DiffBase<T>::rawValue(index));
        } else {
            return fmt::format("{}", DiffBase<T>::rawValue(index));
        }
    }
};

template <typename T>
struct DiffHexFixedWidth : public DiffBase<T> {
    DiffHexFixedWidth(const std::string_view textHeaderIn, const std::string_view jsonPathIn,
        const std::optional<T>& value1, const std::optional<T>& value2)
        : DiffBase<T>(textHeaderIn, jsonPathIn, value1, value2) {}

    DiffHexFixedWidth(const std::string_view textHeaderIn, const std::string_view jsonPathIn,
        const T& value1, const T& value2)
        : DiffBase<T>(textHeaderIn, jsonPathIn, value1, value2) {}

    virtual std::string value(std::size_t index, OutputFormat format) const override {
        if (format == OutputFormat::text) {
            return fmt::format(fmt::format("0x{{:0{}x}}", sizeof(T) << 1), DiffBase<T>::rawValue(index));
        } else {
            return fmt::format("{}", DiffBase<T>::rawValue(index));
        }
    }
};

template <typename T>
struct DiffEnum : public DiffBase<T> {
    DiffEnum(const std::string_view textHeaderIn, const std::string_view jsonPathIn,
        const std::optional<int32_t>& value1, const std::optional<int32_t>& value2,
        const std::function<const char*(std::size_t)>& strFunc)
        : DiffBase<T>(textHeaderIn, jsonPathIn,
            value1.has_value() ? T(*value1) : std::optional<T>(),
            value2.has_value() ? T(*value2) : std::optional<T>()) {
        if (value1.has_value()) enumNames[0] = strFunc(0);
        if (value2.has_value()) enumNames[1] = strFunc(1);
    }

    DiffEnum(const std::string_view textHeaderIn, const std::string_view jsonPathIn, const int32_t& value1, const int32_t& value2,
        const std::function<const char*(std::size_t)>& strFunc)
        : DiffBase<T>(textHeaderIn, jsonPathIn, T(value1), T(value2)) {
        enumNames[0] = strFunc(0);
        enumNames[1] = strFunc(1);
    }

    virtual std::string value(std::size_t index, OutputFormat format) const override {
        if (format == OutputFormat::text) {
            if (enumNames[index]) {
                if (hexInText) {
                    return fmt::format("0x{:x} ({})", uint32_t(DiffBase<T>::rawValue(index)), enumNames[index]);
                } else {
                    return enumNames[index];
                }
            } else {
                return fmt::format("0x{:x}", uint32_t(DiffBase<T>::rawValue(index)));
            }
        } else {
            if (enumNames[index]) {
                return fmt::format("\"{}\"", enumNames[index]);
            } else {
                return fmt::format("{}", int32_t(DiffBase<T>::rawValue(index)));
            }
        }
    }

    DiffEnum<T>& outputHexInText() {
        hexInText = true;
        return *this;
    }

private:
    bool hexInText = false;
    const char* enumNames[2] = { nullptr, nullptr };
};

template <>
struct DiffEnum<ktxSupercmpScheme> : public DiffBase<ktxSupercmpScheme> {
    DiffEnum(const std::string_view textHeaderIn, const std::string_view jsonPathIn, const uint32_t& value1, const uint32_t& value2)
        : DiffBase<ktxSupercmpScheme>(textHeaderIn, jsonPathIn, ktxSupercmpScheme(value1), ktxSupercmpScheme(value2)) {
        enumNames[0] = ktxSupercompressionSchemeString(ktxSupercmpScheme(value1));
        enumNames[1] = ktxSupercompressionSchemeString(ktxSupercmpScheme(value2));
    }

    virtual std::string value(std::size_t index, OutputFormat format) const override {
        if (format == OutputFormat::text) {
            if (enumNames[index]) {
                if (strcmp(enumNames[index], "Invalid scheme value") == 0)
                    return fmt::format("Invalid scheme (0x{:x})", uint32_t(rawValue(index)));
                else if (strcmp(enumNames[index], "Vendor or reserved scheme") == 0)
                    return fmt::format("Vendor or reserved scheme (0x{:x})", uint32_t(rawValue(index)));
                else
                    return enumNames[index];
            } else {
                return fmt::format("0x{:x}", uint32_t(rawValue(index)));
            }
        } else {
            if (enumNames[index]) {
                return fmt::format("\"{}\"", enumNames[index]);
            } else {
                return fmt::format("{}", int32_t(rawValue(index)));
            }
        }
    }

private:
    const char* enumNames[2] = { nullptr, nullptr };
};

struct DiffFlags : DiffBase<uint32_t> {
    DiffFlags(const std::string_view textHeaderIn, const std::string_view jsonPathIn,
        const std::optional<uint32_t>& value1, const std::optional<uint32_t>& value2,
        const char*(*bitToString)(uint32_t, bool))
        : DiffBase<uint32_t>(textHeaderIn, jsonPathIn, value1, value2), toStringFn(bitToString) {}

    DiffFlags(const std::string_view textHeaderIn, const std::string_view jsonPathIn, const uint32_t& value1, const uint32_t& value2,
        const char*(*bitToString)(uint32_t, bool))
        : DiffBase<uint32_t>(textHeaderIn, jsonPathIn, value1, value2), toStringFn(bitToString) {}

    virtual std::string value(std::size_t index, OutputFormat format) const override {
        const auto space = format != OutputFormat::json_mini ?  " " : "";
        const auto quote = format == OutputFormat::text ? "" : "\"";

        std::stringstream formattedValue;
        bool first = true;
        for (uint32_t bitIndex = 0; bitIndex < 32; ++bitIndex) {
            uint32_t bitMask = 1u << bitIndex;
            bool bitValue = (bitMask & DiffBase<uint32_t>::rawValue(index)) != 0;

            const auto comma = (bitValue && std::exchange(first, false)) ? "" : fmt::format(",{}", space);
            const char* bitStr = toStringFn(bitIndex, bitValue);
            if (bitStr) {
                fmt::print(formattedValue, "{}{}{}{}", comma, quote, bitStr, quote);
            } else if (bitValue) {
                fmt::print(formattedValue, "{}{}", comma, bitMask);
            }
        }

        if (format == OutputFormat::text) {
            return fmt::format("0x{:x} ({})", DiffBase<uint32_t>::rawValue(index), formattedValue.str());
        } else {
            if (formattedValue.str().empty()) {
                return "[]";
            } else {
                return fmt::format("[{}{}{}]", space, formattedValue.str(), space);
            }
        }
    }

private:
    const char*(*toStringFn)(uint32_t, bool);
};

template <typename T, std::size_t N>
struct DiffArray : public DiffBase<std::array<T, N>> {
    DiffArray(const std::string_view textHeaderIn, const std::string_view jsonPathIn,
        const std::optional<std::array<T, N>>& value1, const std::optional<std::array<T, N>>& value2)
        : DiffBase<std::array<T, N>>(textHeaderIn, jsonPathIn, value1, value2) {}

    DiffArray(const std::string_view textHeaderIn, const std::string_view jsonPathIn,
        const std::array<T, N>& value1, const std::array<T, N>& value2)
        : DiffBase<std::array<T, N>>(textHeaderIn, jsonPathIn, value1, value2) {}

    virtual std::string value(std::size_t index, OutputFormat format) const override {
        const auto space = format != OutputFormat::json_mini ?  " " : "";

        std::stringstream formattedValue;
        bool first = true;
        for (std::size_t arrayIndex = 0; arrayIndex < N; ++arrayIndex) {
            const auto comma = std::exchange(first, false) ? "" : fmt::format(",{}", space);
            fmt::print(formattedValue, "{}{}", comma, DiffBase<std::array<T, N>>::rawValue(index)[arrayIndex]);
        }

        if (format == OutputFormat::text) {
            return formattedValue.str();
        } else {
            if (formattedValue.str().empty()) {
                return "[]";
            } else {
                return fmt::format("[{}{}{}]", space, formattedValue.str(), space);
            }
        }
    }
};

struct DiffRawBytes : public DiffBase<std::vector<uint8_t>> {
    DiffRawBytes(const std::string_view textHeaderIn, const std::string_view jsonPathIn,
        const std::optional<std::vector<uint8_t>>& value1, const std::optional<std::vector<uint8_t>>& value2)
        : DiffBase<std::vector<uint8_t>>(textHeaderIn, jsonPathIn, value1, value2) {}

    DiffRawBytes(const std::string_view textHeaderIn, const std::string_view jsonPathIn,
        const std::vector<uint8_t>& value1, const std::vector<uint8_t>& value2)
        : DiffBase<std::vector<uint8_t>>(textHeaderIn, jsonPathIn, value1, value2) {}

    virtual std::string value(std::size_t index, OutputFormat format) const override {
        const auto space = format != OutputFormat::json_mini ?  " " : "";

        std::stringstream formattedValue;
        bool first = true;
        for (std::size_t arrayIndex = 0; arrayIndex < rawValue(index).size(); ++arrayIndex) {
            const auto comma = std::exchange(first, false) ? "" : fmt::format(",{}", space);
            if (format == OutputFormat::text) {
                fmt::print(formattedValue, "{}0x{:x}", comma, rawValue(index)[arrayIndex]);
            } else {
                fmt::print(formattedValue, "{}{}", comma, rawValue(index)[arrayIndex]);
            }
        }

        if (format == OutputFormat::text) {
            return fmt::format("[{}]", formattedValue.str());
        } else {
            if (formattedValue.str().empty()) {
                return "[]";
            } else {
                return fmt::format("[{}{}{}]", space, formattedValue.str(), space);
            }
        }
    }
};

template <typename T>
struct DiffComplex {
    DiffComplex(const std::string_view textHeaderIn, const std::string_view jsonPathIn,
        const std::optional<T>& value1, const std::optional<T>& value2)
        : textHeader(textHeaderIn), jsonPath(jsonPathIn), values(),
          different((value1.has_value() && value2.has_value())
            ? value1->isDifferent(*value2)
            : (!value1.has_value() || !value2.has_value()) ? true : false) {
        values[0] = value1;
        values[1] = value2;
    }

    DiffComplex(const std::string_view textHeaderIn, const std::string_view jsonPathIn,
        const T& value1, const T& value2)
        : textHeader(textHeaderIn), jsonPath(jsonPathIn), values(), different(value1.different(value2)) {
        values[0] = value1;
        values[1] = value2;
    }

    const std::string_view textHeader;
    const std::string_view jsonPath;

    bool isDifferent() const {
        return different;
    }

    bool hasValue(std::size_t index) const {
        return values[index].has_value();
    }

    void printText(std::size_t index, PrintIndent& out, const char* prefix) const {
        values[index]->printText(out, prefix);
    }

    void printJson(std::size_t index, PrintIndent& out, int indent, const char* space, const char* nl) const {
        values[index]->printJson(out, indent, space, nl);
    }

private:
    std::optional<T> values[2];
    const bool different;
};


struct DiffTextCustom {
    DiffTextCustom(const std::optional<std::string>& text1, const std::optional<std::string>& text2)
        : texts(), different(text1 != text2) {
        texts[0] = text1;
        texts[1] = text2;
    }

    DiffTextCustom(std::string&& text1, std::string&& text2)
        : texts(), different(text1 != text2) {
        texts[0] = text1;
        texts[1] = text2;
    }

    bool isDifferent() const {
        return different;
    }

    bool hasText(std::size_t index) const {
        return texts[index].has_value();
    }

    const std::string& text(std::size_t index) const {
        return *texts[index];
    }

private:
    std::optional<std::string> texts[2];
    const bool different;
};

// Helper used to report a mismatch without actual values to include in the report
struct DiffMismatch {
    DiffMismatch(const std::string_view textMsgIn, const std::string_view jsonPathIn)
        : textMsg(textMsgIn), jsonPath(jsonPathIn) {}

    const std::string_view textMsg;
    const std::string_view jsonPath;
};

class PrintDiff {
    PrintIndent& printIndent;
    OutputFormat outputFormat;
    std::vector<std::string> context;
    bool firstContext;
    bool different;

    void printContext() {
        if (!context.empty()) {
            if (!std::exchange(firstContext, false))
                printIndent(0, "\n");
            for (const auto& ctx : context)
                printIndent(0, ctx);
            context.clear();
        }
    }

public:
    PrintDiff(PrintIndent& output, OutputFormat format)
        : printIndent(output), outputFormat(format), context(), firstContext(true), different(false) {}

    bool isDifferent() const {
        return different;
    }

    void setContext(std::string&& ctx) {
        context.clear();
        context.emplace_back(std::move(ctx));
    }

    void addContext(std::string&& ctx) {
        context.emplace_back(std::move(ctx));
    }

    void updateContext(std::string&& ctx) {
        if (!context.empty()) context.pop_back();
        context.emplace_back(std::move(ctx));
    }

    template <typename T>
    void operator<<(const DiffBase<T>& diff) {
        if (!diff.isDifferent()) return;
        different = true;

        const auto space = outputFormat != OutputFormat::json_mini ?  " " : "";
        const auto nl = outputFormat != OutputFormat::json_mini ?  "\n" : "";

        if (outputFormat == OutputFormat::text) {
            printContext();
            if (diff.hasValue(0))
                printIndent(0, "-{}: {}\n", diff.textHeader, diff.value(0, outputFormat));
            if (diff.hasValue(1))
                printIndent(0, "+{}: {}\n", diff.textHeader, diff.value(1, outputFormat));
        } else {
            printIndent(0, ",{}", nl);
            printIndent(1, "\"{}\":{}[{}", diff.jsonPath, space, nl);
            if (diff.hasValue(0))
                printIndent(2, "{},{}", diff.value(0, outputFormat), nl);
            else
                printIndent(2, "null,{}", nl);
            if (diff.hasValue(1))
                printIndent(2, "{}{}", diff.value(1, outputFormat), nl);
            else
                printIndent(2, "null{}", nl);
            printIndent(1, "]");
        }
    }

    template <typename T>
    void operator<<(const DiffComplex<T>& diff) {
        if (!diff.isDifferent()) return;
        different = true;

        const auto space = outputFormat != OutputFormat::json_mini ?  " " : "";
        const auto nl = outputFormat != OutputFormat::json_mini ?  "\n" : "";

        if (outputFormat == OutputFormat::text) {
            printContext();
            if (diff.hasValue(0)) {
                printIndent(0, "-{}:", diff.textHeader);
                diff.printText(0, printIndent, "-");
            }
            if (diff.hasValue(1)) {
                printIndent(0, "+{}:", diff.textHeader);
                diff.printText(1, printIndent, "+");
            }
        } else {
            printIndent(0, ",{}", nl);
            printIndent(1, "\"{}\":{}[{}", diff.jsonPath, space, nl);
            if (diff.hasValue(0)) {
                diff.printJson(0, printIndent, 2, space, nl);
                printIndent(0, ",{}", nl);
            } else
                printIndent(2, "null,{}", nl);
            if (diff.hasValue(1)) {
                diff.printJson(1, printIndent, 2, space, nl);
                printIndent(0, "{}", nl);
            } else
                printIndent(2, "null{}", nl);
            printIndent(1, "]");
        }
    }

    void operator<<(const DiffTextCustom& diff) {
        if (!diff.isDifferent()) return;
        different = true;

        assert(outputFormat == OutputFormat::text);
        if (diff.hasText(0))
            printIndent(0, "-{}\n", diff.text(0));
        if (diff.hasText(1))
            printIndent(0, "+{}\n", diff.text(1));
    }

    void operator<<(const DiffMismatch& diff) {
        different = true;

        const auto space = outputFormat != OutputFormat::json_mini ?  " " : "";
        const auto nl = outputFormat != OutputFormat::json_mini ?  "\n" : "";

        if (outputFormat == OutputFormat::text) {
            printIndent(0, "+{}\n", diff.textMsg);
        } else {
            printIndent(0, ",{}", nl);
            printIndent(1, "\"{}\":{}[]", diff.jsonPath, space);
        }
    }
};

// -------------------------------------------------------------------------------------------------

/** @page ktx_compare ktx compare
@~English

Compare two KTX2 files.

@section ktx_compare_synopsis SYNOPSIS
    ktx compare [option...] @e input-file1 @e input-file2

@section ktx_compare_description DESCRIPTION
    @b ktx @compare compares the two KTX2 files specified as the @e input-file1 and @e input-file2
    arguments and outputs any mismatch in texture information and/or image data.
    The command implicitly calls @ref ktx_validate "validate" and prints any found errors
    and warnings to stdout.
    If any of the specified input files are invalid then comparison is done based on best effort
    and may be incomplete.

    The JSON output formats conform to the https://schema.khronos.org/ktx/compare_v0.json
    schema even if the input file is invalid and certain information cannot be parsed or
    displayed.
    Additionally, for JSON outputs the KTX file identifier is printed using "\u001A" instead of
    "\x1A" as an unescaped "\x1A" sequence inside a JSON string breaks nearly every JSON tool.
    Note that this does not change the value of the string only its representation.

    @note @b ktx @b compare prints using UTF-8 encoding. If your console is not
    set for UTF-8 you will see incorrect characters in output of the file
    identifier on each side of the "KTX nn".

    The following options are available:
    @snippet{doc} ktx/command.h command options_format
    <dl>
        <dt>\--content raw | image | ignore</dt>
        <dd>Controls how image content is compared. Possible options are: <br />
            @b raw - Encoded image data is compared verbatim, as it appears in the file. <br />
            @b image - Effective image data is compared per texel block. <br />
            @b ignore - Ignore image contents. <br />
            The default mode is @b raw, meaning that the encoded image data must match exactly.
        </dd>
        <dt>\--allow-invalid-input</dt>
        <dd>Perform best effort comparison even if any of the input files are invalid.</dd>
        <dt>\--ignore-supercomp</dt>
        <dd>Ignore supercompression scheme in the file header. <br />
            Note: use the --ignore-sgd option to also ignore the SGD section, if needed.</dd>
        <dt>\--ignore-index all | level | none</dt>
        <dd>Controls the comparison of index entries in the file headers. Possible options are: <br />
            @b all - Ignore all index entries. <br />
            @b level - Ignore level index entries only. <br />
            @b none - Do not ignore any index entries. <br />
            The default mode is @b none, meaning that all index entries will be compared.
        </dd>
        <dt>\--ignore-dfd all | unknown | extended | none</dt>
        <dd>Controls the comparison of DFD blocks. Possible options are: <br />
            @b all - Ignore all DFD blocks. <br />
            @b unknown - Ignore any unrecognized DFD blocks. <br />
            @b extended - Ignore all DFD blocks except the basic DFD block. <br />
            @b none - Do not ignore any DFD blocks. <br />
            The default mode is @b none, meaning that all DFD entries will be compared.
        </dd>
        <dt>\--ignore-metadata all | &lt;keys&gt; | none</dt>
        <dd>Controls the comparison of metadata (KVD) entries. Possible options are: <br />
            @b all - Ignore all metadata entries. <br />
            @b &lt;keys&gt; - Ignore the specified comma separated list of metadata keys. <br />
            @b none - Do not ignore any metadata entries. <br />
            The default mode is @b none, meaning that all metadata entries will be compared.
        </dd>
        <dt>\--ignore-sgd all | unknown | payload | none</dt>
        <dd>Controls the comparison of the SGD section. Possible options are: <br />
            @b all - Ignore the SGD section. <br />
            @b unknown - Ignore any unrecognized SGD section. <br />
            @b payload - Ignore any unrecognized SGD section and the payload of any known SGD section. <br />
            @b none - Do not ignore the SGD section. <br />
            The default mode is @b none, meaning that SGD sections will be always compared.
        </dd>
    </dl>
    @snippet{doc} ktx/command.h command options_generic

@section ktx_compare_exitstatus EXIT STATUS
    @snippet{doc} ktx/command.h command exitstatus
    - 7 - Input files are different

@section ktx_compare_history HISTORY

@par Version 4.0
 - Initial version

@section ktx_compare_author AUTHOR
    - Daniel Rákos, RasterGrid www.rastergrid.com
*/
class CommandCompare : public Command {
    enum class ContentMode {
        raw,
        image,
        ignore
    };

    enum class IgnoreIndex {
        all,
        level,
        none
    };

    enum class IgnoreDFD {
        all,
        unknown,
        extended,
        none
    };

    enum class IgnoreSGD {
        all,
        unknown,
        payload,
        none
    };

    struct OptionsCompare {
        inline static const char* kContent = "content";
        inline static const char* kAllowInvalidInput = "allow-invalid-input";
        inline static const char* kIgnoreSupercomp = "ignore-supercomp";
        inline static const char* kIgnoreIndex = "ignore-index";
        inline static const char* kIgnoreDFD = "ignore-dfd";
        inline static const char* kIgnoreMetadata = "ignore-metadata";
        inline static const char* kIgnoreSGD = "ignore-sgd";

        std::array<std::string, 2> inputFilepaths;
        ContentMode contentMode = ContentMode::raw;
        bool allowInvalidInput = false;
        bool ignoreSupercomp = false;
        IgnoreIndex ignoreIndex = IgnoreIndex::none;
        IgnoreDFD ignoreDFD = IgnoreDFD::none;
        bool ignoreAllMetadata = false;
        std::unordered_set<std::string> ignoreMetadataKeys = {};
        IgnoreSGD ignoreSGD = IgnoreSGD::none;

        void init(cxxopts::Options& opts) {
            opts.add_options()
                    ("input-file1", "The first input file to compare.", cxxopts::value<std::string>(), "filepath")
                    ("input-file2", "The second input file to compare.", cxxopts::value<std::string>(), "filepath")
                    (kContent, "Controls how image content is compared. Possible values are:\n"
                        "  raw: Encoded image data is compared verbatim, as it appears in the file\n"
                        "  image: Effective image data is compared per texel block\n"
                        "  ignore: Ignore image contents\n",
                        cxxopts::value<std::string>()->default_value("raw"), "raw|image|ignore")
                    (kAllowInvalidInput, "Perform best effort comparison even if any of the input files are invalid.")
                    (kIgnoreSupercomp, "Ignore supercompression scheme in the file header.\n"
                        "Note: use the --ignore-sgd option to also ignore the SGD section, if needed.")
                    (kIgnoreIndex, "Controls the comparison of index entries in the file headers. Possible options are:\n"
                        "  all: Ignore all index entries\n"
                        "  level: Ignore level index entries only\n"
                        "  none: Do not ignore any index entries\n",
                        cxxopts::value<std::string>()->default_value("none"), "all|level|none")
                    (kIgnoreDFD, "Controls the comparison of DFD blocks. Possible options are:\n"
                        "  all: Ignore all DFD blocks\n"
                        "  unknown: Ignore any unrecognized DFD blocks\n"
                        "  extended: Ignore all DFD blocks except the basic DFD block\n"
                        "  none: Do not ignore any DFD blocks\n",
                        cxxopts::value<std::string>()->default_value("none"), "all|unknown|extended|none")
                    (kIgnoreMetadata, "Controls the comparison of metadata (KVD) entries. Possible options are:\n"
                        "  all: Ignore all metadata entries\n"
                        "  <keys>: Ignore the specified comma separated list of metadata keys\n"
                        "  none: Do not ignore any metadata entries\n",
                        cxxopts::value<std::string>()->default_value("none"), "all|<keys>|none")
                    (kIgnoreSGD, "Controls the comparison of the SGD section. Possible options are:\n"
                        "  all: Ignore the SGD section\n"
                        "  unknown: Ignore any unrecognized SGD section\n"
                        "  payload: Ignore any unrecognized SGD section and the payload of any known SGD section\n"
                        "  none: Do not ignore the SGD section\n",
                        cxxopts::value<std::string>()->default_value("none"), "all|unknown|payload|none");
            opts.parse_positional("input-file1", "input-file2");
            opts.positional_help("<input-file1> <input-file2>");
        }

        void process(cxxopts::Options&, cxxopts::ParseResult& args, Reporter& report) {
            if (args.count("input-file1") == 0)
                report.fatal_usage("Missing input files.");
            if (args.count("input-file2") == 0)
                report.fatal_usage("Missing second input file.");

            inputFilepaths[0] = args["input-file1"].as<std::string>();
            inputFilepaths[1] = args["input-file2"].as<std::string>();

            if (args[kContent].count()) {
                static std::unordered_map<std::string, ContentMode> contentModeMapping{
                    {"raw", ContentMode::raw},
                    {"image", ContentMode::image},
                    {"ignore", ContentMode::ignore}
                };
                const auto contentModeStr = to_lower_copy(args[kContent].as<std::string>());
                const auto it = contentModeMapping.find(contentModeStr);
                if (it == contentModeMapping.end())
                    report.fatal_usage("Invalid --content argument: \"{}\".", contentModeStr);
                contentMode = it->second;
            }

            allowInvalidInput = args[kAllowInvalidInput].as<bool>();

            ignoreSupercomp = args[kIgnoreSupercomp].as<bool>();

            if (args[kIgnoreIndex].count()) {
                static std::unordered_map<std::string, IgnoreIndex> ignoreIndexMapping{
                    {"all", IgnoreIndex::all},
                    {"level", IgnoreIndex::level},
                    {"none", IgnoreIndex::none}
                };
                const auto ignoreIndexStr = to_lower_copy(args[kIgnoreIndex].as<std::string>());
                const auto it = ignoreIndexMapping.find(ignoreIndexStr);
                if (it == ignoreIndexMapping.end())
                    report.fatal_usage("Invalid --ignore-index argument: \"{}\".", ignoreIndexStr);
                ignoreIndex = it->second;
            }

            if (args[kIgnoreDFD].count()) {
                static std::unordered_map<std::string, IgnoreDFD> ignoreDFDMapping{
                    {"all", IgnoreDFD::all},
                    {"unknown", IgnoreDFD::unknown},
                    {"extended", IgnoreDFD::extended},
                    {"none", IgnoreDFD::none}
                };
                const auto ignoreDFDStr = to_lower_copy(args[kIgnoreDFD].as<std::string>());
                const auto it = ignoreDFDMapping.find(ignoreDFDStr);
                if (it == ignoreDFDMapping.end())
                    report.fatal_usage("Invalid --ignore-dfd argument: \"{}\".", ignoreDFDStr);
                ignoreDFD = it->second;
            }

            if (args[kIgnoreMetadata].count()) {
                static std::unordered_map<std::string, bool> ignoreMetadataMapping{
                    {"all", true},
                    {"none", false}
                };
                const auto ignoreMetadataStr = to_lower_copy(args[kIgnoreMetadata].as<std::string>());
                const auto it = ignoreMetadataMapping.find(ignoreMetadataStr);
                if (it == ignoreMetadataMapping.end()) {
                    // Comma separated list of keys
                    std::stringstream stream(args[kIgnoreMetadata].as<std::string>());
                    std::string key;
                    while (std::getline(stream, key, ','))
                        ignoreMetadataKeys.emplace(std::move(key));
                } else {
                    ignoreAllMetadata = it->second;
                }
            }

            if (args[kIgnoreSGD].count()) {
                static std::unordered_map<std::string, IgnoreSGD> ignoreSGDMapping{
                    {"all", IgnoreSGD::all},
                    {"unknown", IgnoreSGD::unknown},
                    {"payload", IgnoreSGD::payload},
                    {"none", IgnoreSGD::none}
                };
                const auto ignoreSGDStr = to_lower_copy(args[kIgnoreSGD].as<std::string>());
                const auto it = ignoreSGDMapping.find(ignoreSGDStr);
                if (it == ignoreSGDMapping.end())
                    report.fatal_usage("Invalid --ignore-sgd argument: \"{}\".", ignoreSGDStr);
                ignoreSGD = it->second;
            }
        }
    };

    Combine<OptionsFormat, OptionsCompare, OptionsGeneric> options;

    using InputStreams = std::array<InputStream, 2>;

    std::vector<KTX_header2> headers;

public:
    virtual int main(int argc, char* argv[]) override;
    virtual void initOptions(cxxopts::Options& opts) override;
    virtual void processOptions(cxxopts::Options& opts, cxxopts::ParseResult& args) override;

private:
    void executeCompare();

    void compareHeader(PrintDiff& diff, InputStreams& streams);
    void compareLevelIndex(PrintDiff& diff, InputStreams& streams);
    void compareDFD(PrintDiff& diff, InputStreams& streams);
    void compareDFDBasic(PrintDiff& diff, uint32_t blockIndex,
        std::optional<BDFD> bdfds[2], std::optional<std::vector<SampleType>> bdfdSamples[2]);
    void compareKVD(PrintDiff& diff, InputStreams& streams);
    void compareKVEntry(PrintDiff& diff, const std::string_view& key,
        ktxHashListEntry* entry1, ktxHashListEntry* entry2);
    void compareSGD(PrintDiff& diff, InputStreams& streams);

    void read(InputStream& stream, std::size_t offset, void* readDst, std::size_t readSize, std::string_view what) {
        stream->seekg(offset);
        if (stream->fail())
            fatal(rc::IO_FAILURE, "Failed to seek file to {} \"{}\": {}.", what, stream.str(), errnoMessage());

        stream->read(reinterpret_cast<char*>(readDst), readSize);
        if (stream->eof()) {
            fatal(rc::IO_FAILURE, "Unexpected end of file reading {} from file \"{}\".", what, stream.str());
        } else if (stream->fail()) {
            fatal(rc::IO_FAILURE, "Failed to read {} from file \"{}\": {}.", what, stream.str(), errnoMessage());
        }
    }

    template <typename T>
    std::vector<T> read(InputStreams& streams, std::size_t offset, std::string_view what) {
        std::vector<T> result(streams.size());
        for (std::size_t i = 0; i < streams.size(); ++i) {
            read(streams[i], offset, &result[i], sizeof(T), what);
        }
        return result;
    }
};

// -------------------------------------------------------------------------------------------------

int CommandCompare::main(int argc, char* argv[]) {
    try {
        parseCommandLine("ktx compare",
                "Compares the two KTX files specified as the input-file1 and input-file2 arguments.\n"
                "    The command implicitly calls validate and prints any found errors\n"
                "    and warnings to stdout.",
                argc, argv);
        executeCompare();
        return to_underlying(rc::SUCCESS);
    } catch (const FatalError& error) {
        return +error.returnCode;
    } catch (const std::exception& e) {
        fmt::print(std::cerr, "{} fatal: {}\n", commandName, e.what());
        return +rc::RUNTIME_ERROR;
    }
}

void CommandCompare::initOptions(cxxopts::Options& opts) {
    options.init(opts);
}

void CommandCompare::processOptions(cxxopts::Options& opts, cxxopts::ParseResult& args) {
    options.process(opts, args, *this);
}

void CommandCompare::executeCompare() {
    InputStreams inputStreams{
        InputStream(options.inputFilepaths[0], *this),
        InputStream(options.inputFilepaths[1], *this),
    };

    std::vector<std::string> validationMessages;
    std::vector<int> validationResults;

    switch (options.format) {
    case OutputFormat::text: {
        for (std::size_t i = 0; i < inputStreams.size(); ++i) {
            std::ostringstream messagesOS;
            validationResults.emplace_back(validateIOStream(inputStreams[i], fmtInFile(options.inputFilepaths[i]),
                false, false, [&](const ValidationReport& issue) {
                fmt::print(messagesOS, "{}-{:04}: {}\n", toString(issue.type), issue.id, issue.message);
                fmt::print(messagesOS, "    {}\n", issue.details);
            }));

            validationMessages.emplace_back(std::move(messagesOS).str());
        }

        bool hasValidationError = false;
        for (std::size_t i = 0; i < inputStreams.size(); ++i) {
            if (!validationMessages[i].empty()) {
                if (std::exchange(hasValidationError, true))
                    fmt::print("\n");

                fmt::print("Validation {} for '{}'\n", validationResults[i] == 0 ? "successful" : "failed",
                            options.inputFilepaths[i]);
                fmt::print("\n");
                fmt::print("{}", validationMessages[i]);
            }
        }

        for (std::size_t i = 0; i < inputStreams.size(); ++i) {
            if (validationResults[i] != 0)
                if (validationResults[i] != +rc::INVALID_FILE || !options.allowInvalidInput)
                    throw FatalError(ReturnCode{validationResults[i]});
        }

        if (hasValidationError)
            fmt::print("\n");

        PrintIndent out{std::cout};
        PrintDiff diff(out, options.format);
        compareHeader(diff, inputStreams);
        compareLevelIndex(diff, inputStreams);
        compareDFD(diff, inputStreams);
        compareKVD(diff, inputStreams);
        compareSGD(diff, inputStreams);

        if (diff.isDifferent())
            throw FatalError(rc::DIFFERENCE_FOUND);

        break;
    }
    case OutputFormat::json: [[fallthrough]];
    case OutputFormat::json_mini: {
        int fatalValidationError = 0;

        const auto baseIndent = options.format == OutputFormat::json ? +0 : 0;
        const auto indentWidth = options.format == OutputFormat::json ? 4 : 0;
        const auto space = options.format == OutputFormat::json ? " " : "";
        const auto nl = options.format == OutputFormat::json ? "\n" : "";

        PrintIndent out{std::cout, baseIndent, indentWidth};
        out(0, "{{{}", nl);
        out(1, "\"$schema\":{}\"https://schema.khronos.org/ktx/compare_v0.json\",{}", space, nl);

        for (std::size_t i = 0; i < inputStreams.size(); ++i) {
            std::ostringstream messagesOS;
            PrintIndent pi{messagesOS, baseIndent, indentWidth};

            bool first = true;
            validationResults.emplace_back(validateIOStream(inputStreams[i], fmtInFile(options.inputFilepaths[i]),
                false, false, [&](const ValidationReport& issue) {
                if (!std::exchange(first, false)) {
                    pi(3, "}},{}", nl);
                }
                pi(3, "{{{}", nl);
                pi(4, "\"id\":{}{},{}", space, issue.id, nl);
                pi(4, "\"type\":{}\"{}\",{}", space, toString(issue.type), nl);
                pi(4, "\"message\":{}\"{}\",{}", space, escape_json_copy(issue.message), nl);
                pi(4, "\"details\":{}\"{}\"{}", space, escape_json_copy(issue.details), nl);
            }));

            validationMessages.emplace_back(std::move(messagesOS).str());
        }

        out(1, "\"valid\":{}[{}", space, nl);
        for (std::size_t i = 0; i < inputStreams.size(); ++i) {
            bool last = (i == inputStreams.size() - 1);
            out(2, "{}{}{}", validationResults[i] == 0, last ? "" : ",", nl);
        }
        out(1, "],{}", nl);

        out(1, "\"messages\":{}[{}", space, nl);
        for (std::size_t i = 0; i < inputStreams.size(); ++i) {
            bool last = (i == inputStreams.size() - 1);
            if (!validationMessages[i].empty()) {
                out(2, "[{}", nl);
                fmt::print("{}", validationMessages[i]);
                out(3, "}}{}", nl);
                out(2, "]{}{}", last ? "" : ",", nl);
            } else {
                out(2, "[]{}{}", last ? "" : ",", nl);
            }
        }
        out(1, "]");

        for (std::size_t i = 0; i < inputStreams.size(); ++i) {
            if (validationResults[i] != 0)
                if (validationResults[i] != +rc::INVALID_FILE || !options.allowInvalidInput) {
                    fatalValidationError = validationResults[i];
                    break;
                }
        }

        try {
            if (fatalValidationError)
                throw FatalError(ReturnCode{fatalValidationError});

            PrintDiff diff(out, options.format);
            compareHeader(diff, inputStreams);
            compareLevelIndex(diff, inputStreams);
            compareDFD(diff, inputStreams);
            compareKVD(diff, inputStreams);
            compareSGD(diff, inputStreams);

            if (diff.isDifferent())
                throw FatalError(rc::DIFFERENCE_FOUND);
        } catch (...) {
            fmt::print("{}}}{}", nl, nl);
            throw;
        }
        fmt::print("{}}}{}", nl, nl);
        break;
    }
    }
}

// Helper macros to access the fields of optional structures
#define OPT_FIELD(struct, member) ((struct).has_value() ? \
    std::optional<decltype((struct)->member)>((struct)->member) : std::optional<decltype((struct)->member)>() )
#define OPT_BITFIELD(struct, member) ((struct).has_value() ? \
    std::optional<int32_t>(static_cast<int32_t>((struct)->member)) : std::optional<int32_t>() )
#define OPT_UINT4(struct, memberPrefix) ((struct).has_value() ? \
    std::optional<std::array<uint32_t, 4>>(std::array<uint32_t, 4>{ \
        (struct)->memberPrefix##0, \
        (struct)->memberPrefix##1, \
        (struct)->memberPrefix##2, \
        (struct)->memberPrefix##3, \
    }) : std::optional<std::array<uint32_t, 4>>())
// Helper macros to access the fields of an optional structure array with 2 values
#define OPT_FIELDS(structArr, member) OPT_FIELD((structArr)[0], member), OPT_FIELD((structArr)[1], member)
#define OPT_BITFIELDS(structArr, member) OPT_BITFIELD((structArr)[0], member), OPT_BITFIELD((structArr)[1], member)
#define OPT_UINT4S(structArr, memberPrefix) OPT_UINT4((structArr)[0], memberPrefix), OPT_UINT4((structArr)[1], memberPrefix)

void CommandCompare::compareHeader(PrintDiff& diff, InputStreams& streams) {
    diff.setContext("Header\n\n");

    headers = read<KTX_header2>(streams, 0, "header");

    diff << DiffIdentifier("identifier", "/header/identifier", headers[0], headers[1]);
    diff << DiffEnum<VkFormat>("vkFormat", "/header/vkFormat", headers[0].vkFormat, headers[1].vkFormat,
        [&](auto i) { return vkFormatString(VkFormat(headers[i].vkFormat)); });
    diff << Diff("typeSize", "/header/typeSize", headers[0].typeSize, headers[1].typeSize);
    diff << Diff("pixelWidth", "/header/pixelWidth", headers[0].pixelWidth, headers[1].pixelWidth);
    diff << Diff("pixelHeight", "/header/pixelHeight", headers[0].pixelHeight, headers[1].pixelHeight);
    diff << Diff("pixelDepth", "/header/pixelDepth", headers[0].pixelDepth, headers[1].pixelDepth);
    diff << Diff("layerCount", "/header/layerCount", headers[0].layerCount, headers[1].layerCount);
    diff << Diff("faceCount", "/header/faceCount", headers[0].faceCount, headers[1].faceCount);
    diff << Diff("levelCount", "/header/levelCount", headers[0].levelCount, headers[1].levelCount);

    if (!options.ignoreSupercomp)
        diff << DiffEnum<ktxSupercmpScheme>("supercompressionScheme", "/header/supercompressionScheme",
            headers[0].supercompressionScheme, headers[1].supercompressionScheme);

    if (options.ignoreIndex != IgnoreIndex::all) {
        diff << DiffHex("dataFormatDescriptor.byteOffset", "/index/dataFormatDescriptor/byteOffset",
            headers[0].dataFormatDescriptor.byteOffset, headers[1].dataFormatDescriptor.byteOffset);
        diff << Diff("dataFormatDescriptor.byteLength", "/index/dataFormatDescriptor/byteLength",
            headers[0].dataFormatDescriptor.byteLength, headers[1].dataFormatDescriptor.byteLength);

        diff << DiffHex("keyValueData.byteOffset", "/index/keyValueData/byteOffset",
            headers[0].keyValueData.byteOffset, headers[1].keyValueData.byteOffset);
        diff << Diff("keyValueData.byteLength", "/index/keyValueData/byteLength",
            headers[0].keyValueData.byteLength, headers[1].keyValueData.byteLength);

        diff << DiffHex("supercompressionGlobalData.byteOffset", "/index/supercompressionGlobalData/byteOffset",
            headers[0].supercompressionGlobalData.byteOffset, headers[1].supercompressionGlobalData.byteOffset);
        diff << Diff("supercompressionGlobalData.byteLength", "/index/supercompressionGlobalData/byteLength",
            headers[0].supercompressionGlobalData.byteLength, headers[1].supercompressionGlobalData.byteLength);
    }
}

void CommandCompare::compareLevelIndex(PrintDiff& diff, InputStreams& streams) {
    if (options.ignoreIndex != IgnoreIndex::none) return;

    diff.setContext("Level Index\n\n");

    const uint32_t numLevels[] = {
        std::max(1u, headers[0].levelCount),
        std::max(1u, headers[1].levelCount)
    };
    const uint32_t maxNumLevels = std::max(numLevels[0], numLevels[1]);

    for (uint32_t level = 0; level < maxNumLevels; ++level) {
        const auto levelIndexEntryOffset = sizeof(KTX_header2) + level * sizeof(ktxLevelIndexEntry);
        std::optional<ktxLevelIndexEntry> levelIndexEntry[2];
        for (std::size_t i = 0; i < streams.size(); ++i)
            if (level < numLevels[i]) {
                ktxLevelIndexEntry entry;
                read(streams[i], levelIndexEntryOffset, &entry, sizeof(entry), "the level index");
                levelIndexEntry[i] = entry;
            }

        diff << DiffHex(fmt::format("Level{}.byteOffset", level),
            fmt::format("/index/levels/{}/byteOffset", level),
            OPT_FIELDS(levelIndexEntry, byteOffset));
        diff << Diff(fmt::format("Level{}.byteLength", level),
            fmt::format("/index/levels/{}/byteLength", level),
            OPT_FIELDS(levelIndexEntry, byteLength));
        diff << Diff(fmt::format("Level{}.uncompressedByteLength", level),
            fmt::format("/index/levels/{}/uncompressedByteLength", level),
            OPT_FIELDS(levelIndexEntry, uncompressedByteLength));
    }
}

void CommandCompare::compareDFD(PrintDiff& diff, InputStreams& streams) {
    if (options.ignoreDFD == IgnoreDFD::all) return;

    diff.setContext("Data Format Descriptor\n\n");

    std::unique_ptr<uint8_t[]> buffers[] = {
        std::make_unique<uint8_t[]>(headers[0].dataFormatDescriptor.byteLength),
        std::make_unique<uint8_t[]>(headers[1].dataFormatDescriptor.byteLength)
    };

    for (std::size_t i = 0; i < streams.size(); ++i)
        read(streams[i], headers[i].dataFormatDescriptor.byteOffset, buffers[i].get(),
            headers[i].dataFormatDescriptor.byteLength, "the DFD blocks");

    const uint8_t* ptrDFD[] = { buffers[0].get(), buffers[1].get() };
    const uint8_t* ptrDFDEnd[] = {
        ptrDFD[0] + headers[0].dataFormatDescriptor.byteLength,
        ptrDFD[1] + headers[1].dataFormatDescriptor.byteLength,
    };
    const uint8_t* ptrDFDIt[] = { ptrDFD[0], ptrDFD[1] };

    uint32_t dfdTotalSize[2];
    for (std::size_t i = 0; i < streams.size(); ++i) {
        std::memcpy(&dfdTotalSize[i], ptrDFDIt[i], sizeof(uint32_t));
        ptrDFDIt[i] += sizeof(uint32_t);
    }

    if (dfdTotalSize[0] != dfdTotalSize[1])
        diff << Diff("DFD total bytes", "/dataFormatDescriptor/totalSize", dfdTotalSize[0], dfdTotalSize[1]);

    uint32_t blockIndex = 0;
    while ((ptrDFDIt[0] < ptrDFDEnd[0]) || (ptrDFDIt[1] < ptrDFDEnd[1])) {
        const std::size_t remainingDFDBytes[] = {
            static_cast<std::size_t>(ptrDFDEnd[0] - ptrDFDIt[0]),
            static_cast<std::size_t>(ptrDFDEnd[1] - ptrDFDIt[1]),
        };

        std::optional<DFDHeader> blockHeaders[2] = {};
        if (remainingDFDBytes[0] < sizeof(DFDHeader) && remainingDFDBytes[1] < sizeof(DFDHeader))
            break;

        for (std::size_t i = 0; i < streams.size(); ++i)
            if (remainingDFDBytes[i] >= sizeof(DFDHeader)) {
                DFDHeader blockHeader;
                std::memcpy(&blockHeader, ptrDFDIt[i], sizeof(blockHeader));
                blockHeaders[i] = blockHeader;
            }

        bool dfdKnown[2] = { false, false };
        bool dfdBasic[2] = { false, false };
        for (std::size_t i = 0; i < streams.size(); ++i) {
            if (blockHeaders[i].has_value() &&
                blockHeaders[i]->vendorId == KHR_DF_VENDORID_KHRONOS &&
                blockHeaders[i]->descriptorType == KHR_DF_KHR_DESCRIPTORTYPE_BASICFORMAT) {
                dfdKnown[i] = dfdBasic[i] = true;
            }
        }

        // Consider the ignore-dfd option before comparing the headers
        bool compareDFDs = true;
        switch (options.ignoreDFD) {
            case IgnoreDFD::unknown:
                // Only compare the DFDs if at least one of them is known
                compareDFDs = dfdKnown[0] || dfdKnown[1];
                break;

            case IgnoreDFD::extended:
                // Only compare the DFDs if at least one of them is basic
                compareDFDs = dfdBasic[0] || dfdBasic[1];
                break;

            default:
                break;
        }

        if (compareDFDs) {
            diff << DiffEnum<khr_df_vendorid_e>("Vendor ID",
                fmt::format("/dataFormatDescriptor/blocks/{}/vendorId", blockIndex),
                OPT_BITFIELDS(blockHeaders, vendorId),
                [&](auto i) { return dfdToStringVendorID(khr_df_vendorid_e(blockHeaders[i]->vendorId)); });
            diff << DiffEnum<khr_df_khr_descriptortype_e>("Descriptor type",
                fmt::format("/dataFormatDescriptor/blocks/{}/descriptorType", blockIndex),
                OPT_BITFIELDS(blockHeaders, descriptorType),
                [&](auto i) {
                    return (blockHeaders[i]->vendorId == KHR_DF_VENDORID_KHRONOS)
                        ? dfdToStringDescriptorType(khr_df_khr_descriptortype_e(blockHeaders[i]->descriptorType))
                        : nullptr;
                });
            diff << DiffEnum<khr_df_versionnumber_e>("Version",
                fmt::format("/dataFormatDescriptor/blocks/{}/versionNumber", blockIndex),
                OPT_BITFIELDS(blockHeaders, versionNumber),
                [&](auto i) { return dfdToStringVersionNumber(khr_df_versionnumber_e(blockHeaders[i]->versionNumber)); });
            diff << Diff("Descriptor block size",
                fmt::format("/dataFormatDescriptor/blocks/{}/descriptorBlockSize", blockIndex),
                OPT_BITFIELDS(blockHeaders, descriptorBlockSize));

            // Compare basic DFD data if possible
            if (dfdBasic[0] || dfdBasic[1]) {
                std::optional<BDFD> bdfds[2] = {};
                std::optional<std::vector<SampleType>> bdfdSamples[2] = {};
                for (std::size_t i = 0; i < streams.size(); ++i)
                    if (blockHeaders[i].has_value() &&
                        blockHeaders[i]->vendorId == KHR_DF_VENDORID_KHRONOS &&
                        blockHeaders[i]->descriptorType == KHR_DF_KHR_DESCRIPTORTYPE_BASICFORMAT) {
                        BDFD bdfd;
                        std::memcpy(&bdfd, ptrDFDIt[i], sizeof(bdfd));
                        bdfds[i] = std::move(bdfd);

                        std::vector<SampleType> samples(std::min(MAX_NUM_BDFD_SAMPLES, (blockHeaders[i]->descriptorBlockSize - 24u) / 16u));
                        std::memcpy(samples.data(), ptrDFDIt[i] + sizeof(BDFD), samples.size() * sizeof(SampleType));
                        bdfdSamples[i] = std::move(samples);
                    }

                assert(bdfds[0].has_value() || bdfds[1].has_value());
                compareDFDBasic(diff, blockIndex, bdfds, bdfdSamples);
            }

            // Compare any unrecognized DFD data as raw payload
            if (!dfdKnown[0] || !dfdKnown[1]) {
                std::optional<std::vector<uint8_t>> rawPayloads[2];
                for (std::size_t i = 0; i < streams.size(); ++i) {
                    if (blockHeaders[i].has_value()) {
                        rawPayloads[i] = std::vector<uint8_t>(blockHeaders[i]->descriptorBlockSize - sizeof(DFDHeader));
                        std::memcpy(rawPayloads[i]->data(), ptrDFDIt[i], rawPayloads[i]->size());

                        diff << DiffRawBytes("Raw payload",
                            fmt::format("/dataFormatDescriptor/blocks/{}/rawPayload", blockIndex),
                            rawPayloads[0], rawPayloads[1]);
                    }
                }
            }
        }

        if (++blockIndex >= MAX_NUM_DFD_BLOCKS)
            return;

        for (std::size_t i = 0; i < streams.size(); ++i)
            if (blockHeaders[i].has_value())
                ptrDFDIt[i] += std::max(blockHeaders[i]->descriptorBlockSize, 8u);
    }
}

void CommandCompare::compareDFDBasic(PrintDiff& diff, uint32_t blockIndex,
    std::optional<BDFD> bdfds[2], std::optional<std::vector<SampleType>> bdfdSamples[2]) {
    for (std::size_t i = 0; i < 2; ++i)
        assert(bdfds[i].has_value() == bdfdSamples[i].has_value());

    diff << DiffFlags("Flags",
        fmt::format("/dataFormatDescriptor/blocks/{}/flags", blockIndex),
        OPT_BITFIELDS(bdfds, flags), dfdToStringFlagsBit);
    diff << DiffEnum<khr_df_transfer_e>("Transfer",
        fmt::format("/dataFormatDescriptor/blocks/{}/transferFunction", blockIndex),
        OPT_BITFIELDS(bdfds, transfer),
        [&](auto i) { return dfdToStringTransferFunction(khr_df_transfer_e(bdfds[i]->transfer)); });
    diff << DiffEnum<khr_df_primaries_e>("Primaries",
        fmt::format("/dataFormatDescriptor/blocks/{}/colorPrimaries", blockIndex),
        OPT_BITFIELDS(bdfds, primaries),
        [&](auto i) { return dfdToStringColorPrimaries(khr_df_primaries_e(bdfds[i]->primaries)); });
    diff << DiffEnum<khr_df_model_e>("Model",
        fmt::format("/dataFormatDescriptor/blocks/{}/colorModel", blockIndex),
        OPT_BITFIELDS(bdfds, model),
        [&](auto i) { return dfdToStringColorModel(khr_df_model_e(bdfds[i]->model)); });
    diff << DiffArray("Dimensions",
        fmt::format("/dataFormatDescriptor/blocks/{}/texelBlockDimension", blockIndex),
        OPT_UINT4S(bdfds, texelBlockDimension));
    diff << DiffArray("Plane bytes",
        fmt::format("/dataFormatDescriptor/blocks/{}/bytesPlane", blockIndex),
        OPT_FIELDS(bdfds, bytesPlanes));

    diff.addContext("Sample <i>:\n");

    std::size_t maxNumSamples = std::max(
        bdfdSamples[0].has_value() ? bdfdSamples[0]->size() : 0,
        bdfdSamples[1].has_value() ? bdfdSamples[1]->size() : 0
    );
    for (std::size_t sampleIndex = 0; sampleIndex < maxNumSamples; ++sampleIndex) {
        diff.updateContext(fmt::format("Sample {}:\n", sampleIndex));

        std::optional<SampleType> samples[2] = {};
        for (std::size_t i = 0; i < 2; ++i)
            if (bdfdSamples[i].has_value() && sampleIndex < bdfdSamples[i]->size())
                samples[i] = (*bdfdSamples[i])[sampleIndex];

        std::optional<uint32_t> qualifierFlags[2] = {};
        for (std::size_t i = 0; i < 2; ++i)
            if (samples[i].has_value()) {
                uint32_t flags = 0;
                flags |= samples[i]->qualifierLinear ? KHR_DF_SAMPLE_DATATYPE_LINEAR : 0;
                flags |= samples[i]->qualifierExponent ? KHR_DF_SAMPLE_DATATYPE_EXPONENT : 0;
                flags |= samples[i]->qualifierSigned ? KHR_DF_SAMPLE_DATATYPE_SIGNED : 0;
                flags |= samples[i]->qualifierFloat ? KHR_DF_SAMPLE_DATATYPE_FLOAT : 0;
                qualifierFlags[i] = flags;
            }
        diff << DiffFlags("    Qualifiers",
            fmt::format("/dataFormatDescriptor/blocks/{}/samples/{}/qualifiers", blockIndex, sampleIndex),
            qualifierFlags[0], qualifierFlags[1], dfdToStringSampleDatatypeQualifiersBit);

        diff << DiffEnum<khr_df_model_channels_e>("    Channel Type",
            fmt::format("/dataFormatDescriptor/blocks/{}/samples/{}/channelType", blockIndex, sampleIndex),
            OPT_BITFIELDS(samples, channelType),
            [&](auto i) {
                return dfdToStringChannelId(khr_df_model_e(bdfds[i]->model),
                    khr_df_model_channels_e(samples[i]->channelType));
            })
            .outputHexInText();

        // Text output combines length and offset so we have to special-case here
        if (options.format == OutputFormat::text) {
            std::optional<std::string> lengthAndOffset[2] = {};
            for (std::size_t i = 0; i < 2; ++i)
                if (samples[i].has_value())
                    lengthAndOffset[i] = fmt::format("    Length: {} bits Offset: {}",
                        static_cast<uint32_t>(samples[i]->bitLength),
                        static_cast<uint32_t>(samples[i]->bitOffset));
            diff << DiffTextCustom(lengthAndOffset[0], lengthAndOffset[1]);
        } else {
            diff << Diff({},
                fmt::format("/dataFormatDescriptor/blocks/{}/samples/{}/bitLength", blockIndex, sampleIndex),
                OPT_BITFIELDS(samples, bitLength));
            diff << Diff({},
                fmt::format("/dataFormatDescriptor/blocks/{}/samples/{}/bitOffset", blockIndex, sampleIndex),
                OPT_BITFIELDS(samples, bitOffset));
        }

        diff << DiffArray("    Position",
            fmt::format("/dataFormatDescriptor/blocks/{}/samples/{}/samplePosition", blockIndex, sampleIndex),
            OPT_UINT4S(samples, samplePosition));
        diff << DiffHexFixedWidth("    Lower",
            fmt::format("/dataFormatDescriptor/blocks/{}/samples/{}/sampleLower", blockIndex, sampleIndex),
            OPT_FIELDS(samples, lower));
        diff << DiffHexFixedWidth("    Upper",
            fmt::format("/dataFormatDescriptor/blocks/{}/samples/{}/sampleUpper", blockIndex, sampleIndex),
            OPT_FIELDS(samples, upper));
    }
}

void CommandCompare::compareKVD(PrintDiff& diff, InputStreams& streams) {
    if (options.ignoreAllMetadata) return;

    diff.setContext("Key/Value Data\n\n");

    std::vector<uint8_t> keyValueStores[2] = {};
    std::map<std::string_view, ktxHashListEntry*> keys[2] = {};

    for (std::size_t i = 0; i < streams.size(); ++i) {
        if (headers[i].keyValueData.byteLength == 0) continue;

        keyValueStores[i].resize(headers[i].keyValueData.byteLength);
        read(streams[i], headers[i].keyValueData.byteOffset, keyValueStores[i].data(),
            headers[i].keyValueData.byteLength, "the KVD");

        ktxHashList kvDataHead = nullptr;
        KTX_error_code result = ktxHashList_Deserialize(&kvDataHead,
            headers[i].keyValueData.byteLength, keyValueStores[i].data());
        if (result != KTX_SUCCESS)
            fatal(rc::KTX_FAILURE, "Failed to parse KVD in file \"{}\".", streams[i].str());

        if (kvDataHead == nullptr)
            continue;

        uint32_t entryIndex = 0;
        ktxHashListEntry* entry = kvDataHead;
        for (; entry != NULL && entryIndex < MAX_NUM_KV_ENTRIES; entry = ktxHashList_Next(entry), ++entryIndex) {
            char* key;
            ktx_uint32_t keyLen;
            ktxHashListEntry_GetKey(entry, &keyLen, &key);
            keys[i].emplace(key, entry);
        }
    }

    std::map<std::string_view, ktxHashListEntry*>::iterator it[2] = { keys[0].begin(), keys[1].begin() };
    while (it[0] != keys[0].end() || it[1] != keys[1].end()) {
        bool ignoreKey = false;
        for (std::size_t i = 0; i < streams.size(); ++i)
            if ((it[i] != keys[i].end()) &&
                (options.ignoreMetadataKeys.find(std::string(it[i]->first)) != options.ignoreMetadataKeys.end())) {
                // Key is on the ignore list, skip it
                it[i]++;
                ignoreKey = true;
                break;
            }
        if (ignoreKey) continue;

        if ((it[0] != keys[0].end()) && ((it[1] == keys[1].end()) || (it[0]->first < it[1]->first))) {
            // First stream has a key that is not present in the second stream
            compareKVEntry(diff, it[0]->first, it[0]->second, nullptr);
            it[0]++;
        } else if ((it[1] != keys[1].end()) && ((it[0] == keys[0].end()) || (it[0]->first > it[1]->first))) {
            // Second stream has a key that is not present in the first stream
            compareKVEntry(diff, it[1]->first, nullptr, it[1]->second);
            it[1]++;
        } else {
            // Both streams have the key
            compareKVEntry(diff, it[0]->first, it[0]->second, it[1]->second);
            it[0]++;
            it[1]++;
        }
    }
}

struct KVEntry {
    char* value;
    ktx_uint32_t valueLen;

    template <typename T>
    static std::optional<T> load(ktxHashListEntry* entry) {
        if (entry) {
            T newEntry;
            ktxHashListEntry_GetValue(entry, &newEntry.valueLen, reinterpret_cast<void**>(&newEntry.value));
            if (newEntry.value)
                return std::optional<T>(std::move(newEntry));
        }
        return std::optional<T>();
    }

    virtual bool isDifferent(const KVEntry& other) const {
        return valueLen != other.valueLen || memcmp(value, other.value, valueLen) != 0;
    }

    template <typename T>
    T extract(std::size_t offset = 0) const {
        return (valueLen >= offset + sizeof(T)) ? T(*reinterpret_cast<const T*>(value + offset)) : T();
    }

    std::string extractRawBytes(bool text, const char* space = " ") const {
        std::stringstream formattedValue;
        bool first = true;
        for (ktx_uint32_t arrayIndex = 0; arrayIndex < valueLen; ++arrayIndex) {
            const auto comma = std::exchange(first, false) ? "" : fmt::format(",{}", space);
            if (text) {
                fmt::print(formattedValue, "{}0x{:x}", comma, (uint8_t)value[arrayIndex]);
            } else {
                fmt::print(formattedValue, "{}{}", comma, (uint8_t)value[arrayIndex]);
            }
        }

        if (text) {
            return fmt::format("[{}]", formattedValue.str());
        } else {
            if (formattedValue.str().empty()) {
                return "[]";
            } else {
                return fmt::format("[{}{}{}]", space, formattedValue.str(), space);
            }
        }
    }
};

void CommandCompare::compareKVEntry(PrintDiff& diff, const std::string_view& key,
    ktxHashListEntry* entry1, ktxHashListEntry* entry2) {
    const std::unordered_set<std::string_view> keysWithUint32Values = {
        "KTXdxgiFormat__",
        "KTXmetalPixelFormat"
    };
    const std::unordered_set<std::string_view> keysWithStringValues = {
        "KTXorientation",
        "KTXswizzle",
        "KTXwriter",
        "KTXwriterScParams",
        "KTXastcDecodeMode"
    };

    if (keysWithUint32Values.find(key) != keysWithUint32Values.end()) {
        struct KVEntryUint32 : public KVEntry {
            bool isValid() const {
                return valueLen == sizeof(ktx_uint32_t);
            }

            void printText(PrintIndent& out, const char*) const {
                if (isValid()) {
                    out(0, " {}\n", extract<ktx_uint32_t>());
                } else {
                    out(0, " {}\n", extractRawBytes(true));
                }
            }

            void printJson(PrintIndent& out, int indent, const char* space, const char*) const {
                if (isValid()) {
                    out(indent, "{}", extract<ktx_uint32_t>());
                } else {
                    out(indent, "{}", extractRawBytes(false, space));
                }
            }
        };

        diff << DiffComplex(key, fmt::format("/keyValueData/{}", key),
            KVEntry::load<KVEntryUint32>(entry1), KVEntry::load<KVEntryUint32>(entry2));
    } else if (keysWithStringValues.find(key) != keysWithStringValues.end()) {
        struct KVEntryString : public KVEntry {
            bool isValid() const {
                return value[valueLen - 1] == '\0';
            }

            void printText(PrintIndent& out, const char*) const {
                if (isValid()) {
                    out(0, " {}\n", value);
                } else {
                    out(0, " {}\n", extractRawBytes(true));
                }
            }

            void printJson(PrintIndent& out, int indent, const char* space, const char*) const {
                if (isValid()) {
                    out(indent, "\"{}\"", value);
                } else {
                    out(indent, "{}", extractRawBytes(false, space));
                }
            }
        };

        diff << DiffComplex(key, fmt::format("/keyValueData/{}", key),
            KVEntry::load<KVEntryString>(entry1), KVEntry::load<KVEntryString>(entry2));
    } else if (key == "KTXglFormat") {
        struct KTXglFormat : public KVEntry {
            bool isValid() const {
                return valueLen == 3 * sizeof(ktx_uint32_t);
            }

            void printText(PrintIndent& out, const char* prefix) const {
                if (isValid()) {
                    out(0, "\n");
                    out(0, "{}    glInternalformat: 0x{:08X}\n", prefix, extract<ktx_uint32_t>(0));
                    out(0, "{}    glFormat: 0x{:08X}\n", prefix, extract<ktx_uint32_t>(4));
                    out(0, "{}    glType: 0x{:08X}\n", prefix, extract<ktx_uint32_t>(8));
                } else {
                    out(0, " {}\n", extractRawBytes(true));
                }
            }

            void printJson(PrintIndent& out, int indent, const char* space, const char* nl) const {
                if (isValid()) {
                    out(indent, "{{{}", nl);
                    out(indent + 1, "\"glInternalformat\":{}{},{}", space, extract<ktx_uint32_t>(0), nl);
                    out(indent + 1, "\"glFormat\":{}{},{}", space, extract<ktx_uint32_t>(4), nl);
                    out(indent + 1, "\"glType\":{}{}{}", space, extract<ktx_uint32_t>(8), nl);
                    out(indent, "}}");
                } else {
                    out(indent, "{}", extractRawBytes(false, space));
                }
            }
        };

        diff << DiffComplex("KTXglFormat", "/keyValueData/KTXglFormat",
            KVEntry::load<KTXglFormat>(entry1), KVEntry::load<KTXglFormat>(entry2));
    } else if (key == "KTXanimData") {
        struct KTXanimData : public KVEntry {
            bool isValid() const {
                return valueLen == 3 * sizeof(ktx_uint32_t);
            }

            void printText(PrintIndent& out, const char* prefix) const {
                if (isValid()) {
                    out(0, "\n");
                    out(0, "{}    duration: {}\n", prefix, extract<ktx_uint32_t>(0));
                    out(0, "{}    timescale: {}\n", prefix, extract<ktx_uint32_t>(4));
                    out(0, "{}    loopCount: {}\n", prefix, extract<ktx_uint32_t>(8));
                } else {
                    out(0, " {}\n", extractRawBytes(true));
                }
            }

            void printJson(PrintIndent& out, int indent, const char* space, const char* nl) const {
                if (isValid()) {
                    out(indent, "{{{}", nl);
                    out(indent + 1, "\"duration\":{}{},{}", space, extract<ktx_uint32_t>(0), nl);
                    out(indent + 1, "\"timescale\":{}{},{}", space, extract<ktx_uint32_t>(4), nl);
                    out(indent + 1, "\"loopCount\":{}{}{}", space, extract<ktx_uint32_t>(8), nl);
                    out(indent, "}}");
                } else {
                    out(indent, "{}", extractRawBytes(false, space));
                }
            }
        };

        diff << DiffComplex("KTXanimData", "/keyValueData/KTXanimData",
            KVEntry::load<KTXanimData>(entry1), KVEntry::load<KTXanimData>(entry2));
    } else if (key == "KTXcubemapIncomplete") {
        struct KTXcubemapIncomplete : public KVEntry {
            bool isValid() const {
                return valueLen == sizeof(ktx_uint8_t);
            }

            const char* bitValue(uint8_t bitIndex) const {
                assert(isValid());
                ktx_uint8_t faces = *reinterpret_cast<ktx_uint8_t*>(value);
                return (faces & (1u << bitIndex)) ? "true" : "false;";
            }

            void printText(PrintIndent& out, const char* prefix) const {
                if (isValid()) {
                    out(0, "\n");
                    out(0, "{}    positiveX: {}\n", prefix, bitValue(0));
                    out(0, "{}    negativeX: {}\n", prefix, bitValue(1));
                    out(0, "{}    positiveY: {}\n", prefix, bitValue(2));
                    out(0, "{}    negativeY: {}\n", prefix, bitValue(3));
                    out(0, "{}    positiveZ: {}\n", prefix, bitValue(4));
                    out(0, "{}    negativeZ: {}\n", prefix, bitValue(5));
                } else {
                    out(0, " {}\n", extractRawBytes(true));
                }
            }

            void printJson(PrintIndent& out, int indent, const char* space, const char* nl) const {
                if (isValid()) {
                    out(indent, "{{{}", nl);
                    out(indent + 1, "\"positiveX\":{}{},{}", space, bitValue(0), nl);
                    out(indent + 1, "\"negativeX\":{}{},{}", space, bitValue(1), nl);
                    out(indent + 1, "\"positiveY\":{}{},{}", space, bitValue(2), nl);
                    out(indent + 1, "\"negativeY\":{}{},{}", space, bitValue(3), nl);
                    out(indent + 1, "\"positiveZ\":{}{},{}", space, bitValue(4), nl);
                    out(indent + 1, "\"negativeZ\":{}{},{}", space, bitValue(5), nl);
                    out(indent, "}}");
                } else {
                    out(indent, "{}", extractRawBytes(false, space));
                }
            }
        };

        diff << DiffComplex("KTXcubemapIncomplete", "/keyValueData/KTXcubemapIncomplete",
            KVEntry::load<KTXcubemapIncomplete>(entry1), KVEntry::load<KTXcubemapIncomplete>(entry2));
    } else {
        struct KVEntryUnknown : public KVEntry {
            void printText(PrintIndent& out, const char*) const {
                out(0, " {}\n", extractRawBytes(true));
            }

            void printJson(PrintIndent& out, int indent, const char* space, const char*) const {
                out(indent, "{}", extractRawBytes(false, space));
            }
        };

        diff << DiffComplex(key, fmt::format("/keyValueData/{}", key),
            KVEntry::load<KVEntryUnknown>(entry1), KVEntry::load<KVEntryUnknown>(entry2));
    }
}

void CommandCompare::compareSGD(PrintDiff& diff, InputStreams& streams) {
    if (options.ignoreSGD == IgnoreSGD::all) return;

    auto sgdTypeBasisLZ = [&](std::size_t i) { return headers[i].supercompressionScheme == KTX_SS_BASIS_LZ; };

    std::unique_ptr<uint8_t[]> buffers[] = {
        std::make_unique<uint8_t[]>(headers[0].supercompressionGlobalData.byteLength),
        std::make_unique<uint8_t[]>(headers[1].supercompressionGlobalData.byteLength)
    };

    for (std::size_t i = 0; i < streams.size(); ++i)
        read(streams[i], headers[i].supercompressionGlobalData.byteOffset, buffers[i].get(),
            headers[i].supercompressionGlobalData.byteLength, "the SGD");

    // BasisLZ specific parameters
    struct SGDBasisLZ {
        std::optional<uint16_t> endpointCount;
        std::optional<uint16_t> selectorCount;
        std::optional<uint64_t> endpointsByteLength;
        std::optional<uint64_t> selectorsByteLength;
        std::optional<uint64_t> tablesByteLength;
        std::optional<uint64_t> extendedByteLength;
        std::vector<std::optional<uint32_t>> imageFlags;
        std::vector<std::optional<uint32_t>> rgbSliceByteOffset;
        std::vector<std::optional<uint32_t>> rgbSliceByteLength;
        std::vector<std::optional<uint32_t>> alphaSliceByteOffset;
        std::vector<std::optional<uint32_t>> alphaSliceByteLength;

        std::optional<uint64_t> endpointsByteOffset;
        std::optional<uint64_t> selectorsByteOffset;
        std::optional<uint64_t> tablesByteOffset;
        std::optional<uint64_t> extendedByteOffset;
    } basisLZ[2] = {};

    for (std::size_t i = 0; i < streams.size(); ++i) {
        const auto sgdByteLength = headers[i].supercompressionGlobalData.byteLength;
        if (sgdByteLength == 0) continue;

        read(streams[i], headers[i].supercompressionGlobalData.byteOffset, buffers[i].get(), sgdByteLength, "the SGD");

        if (sgdTypeBasisLZ(i)) {
            uint32_t imageCount = 0;
            for (uint32_t level = 0; level < std::max(headers[i].levelCount, 1u); ++level)
                // numFaces * depth is only reasonable because they can't both be > 1. There are no 3D cubemaps
                imageCount += std::max(headers[i].layerCount, 1u) * headers[i].faceCount * std::max(headers[i].pixelDepth >> level, 1u);

            if (sgdByteLength < sizeof(ktxBasisLzGlobalHeader))
                continue;

            const ktxBasisLzGlobalHeader& bgh = *reinterpret_cast<const ktxBasisLzGlobalHeader*>(buffers[i].get());

            basisLZ[i].endpointCount = bgh.endpointCount;
            basisLZ[i].selectorCount = bgh.selectorCount;
            basisLZ[i].endpointsByteLength = bgh.endpointsByteLength;
            basisLZ[i].selectorsByteLength = bgh.selectorsByteLength;
            basisLZ[i].tablesByteLength = bgh.tablesByteLength;
            basisLZ[i].extendedByteLength = bgh.extendedByteLength;

            if (sgdByteLength < sizeof(ktxBasisLzGlobalHeader) + sizeof(ktxBasisLzEtc1sImageDesc) * imageCount)
                continue;

            const ktxBasisLzEtc1sImageDesc* imageDesc = BGD_ETC1S_IMAGE_DESCS(buffers[i].get());

            for (uint32_t level = 0; level < std::max(headers[i].levelCount, 1u); ++level) {
                for (uint32_t layer = 0; layer < std::max(headers[i].layerCount, 1u); ++layer) {
                    for (uint32_t face = 0; face < headers[i].faceCount; ++face) {
                        for (uint32_t zSlice = 0; zSlice < std::max(headers[i].pixelDepth >> level, 1u); ++zSlice) {
                            basisLZ[i].imageFlags.push_back(imageDesc->imageFlags);
                            basisLZ[i].rgbSliceByteOffset.push_back(imageDesc->rgbSliceByteOffset);
                            basisLZ[i].rgbSliceByteLength.push_back(imageDesc->rgbSliceByteLength);
                            basisLZ[i].alphaSliceByteLength.push_back(imageDesc->alphaSliceByteLength);
                            basisLZ[i].alphaSliceByteOffset.push_back(imageDesc->alphaSliceByteOffset);
                            imageDesc++;
                        }
                    }
                }
            }

            // Calculate payload offsets
            basisLZ[i].endpointsByteOffset = sizeof(ktxBasisLzGlobalHeader) + sizeof(ktxBasisLzEtc1sImageDesc) * imageCount;
            basisLZ[i].selectorsByteOffset = *basisLZ[i].endpointsByteOffset + *basisLZ[i].endpointsByteLength;
            basisLZ[i].tablesByteOffset = *basisLZ[i].selectorsByteOffset + *basisLZ[i].selectorsByteLength;
            basisLZ[i].extendedByteOffset = *basisLZ[i].tablesByteOffset + *basisLZ[i].tablesByteLength;
        }
    }

    // Helper for comparing SGD payloads
    auto compareSGDPayload = [&](const char* textName, const char* jsonPath,
            std::optional<uint64_t> offset1, std::optional<uint64_t> length1,
            std::optional<uint64_t> offset2, std::optional<uint64_t> length2) {
        bool mismatch = false;

        // If SGD is not present in both files then consider that a mismatch
        if (!offset1.has_value() || !length1.has_value() || !offset2.has_value() || !length2.has_value()) {
            mismatch = true;
        }

        if (!mismatch) {
            // If we have an out of bounds situation then consider that a mismatch
            if ((*offset1 + *length1 > headers[0].supercompressionGlobalData.byteLength) ||
                (*offset2 + *length2 > headers[1].supercompressionGlobalData.byteLength)) {
                mismatch = true;
            }
        }

        if (!mismatch) {
            if (length1 != length2) {
                mismatch = true;
            } else {
                mismatch = (memcmp(buffers[0].get() + *offset1, buffers[1].get() + *offset2, *length1) != 0);
            }
        }

        if (mismatch) {
            diff << DiffMismatch(fmt::format("{} mismatch", textName), jsonPath);
        }
    };

    if (sgdTypeBasisLZ(0) || sgdTypeBasisLZ(1)) {
        diff.setContext("Basis Supercompression Global Data\n\n");

        // Supercompression global data type is only needed in JSON format
        if (options.format != OutputFormat::text)
            diff << DiffEnum<ktxSupercmpScheme>("supercompressionScheme", "/supercompressionGlobalData/type",
                headers[0].supercompressionScheme, headers[1].supercompressionScheme);

        diff << Diff("endpointCount", "/supercompressionGlobalData/endpointCount",
            basisLZ[0].endpointCount, basisLZ[1].endpointCount);
        diff << Diff("selectorCount", "/supercompressionGlobalData/selectorCount",
            basisLZ[0].selectorCount, basisLZ[1].selectorCount);
        diff << Diff("endpointsByteLength", "/supercompressionGlobalData/endpointsByteLength",
            basisLZ[0].endpointsByteLength, basisLZ[1].endpointsByteLength);
        diff << Diff("selectorsByteLength", "/supercompressionGlobalData/selectorsByteLength",
            basisLZ[0].selectorsByteLength, basisLZ[1].selectorsByteLength);
        diff << Diff("tablesByteLength", "/supercompressionGlobalData/tablesByteLength",
            basisLZ[0].tablesByteLength, basisLZ[1].tablesByteLength);
        diff << Diff("extendedByteLength", "/supercompressionGlobalData/extendedByteLength",
            basisLZ[0].extendedByteLength, basisLZ[1].extendedByteLength);

        // Make the per image arrays the same size for easier diffing
        std::size_t maxImageCount = std::max(basisLZ[0].imageFlags.size(), basisLZ[1].imageFlags.size());
        for (std::size_t i = 0; i < streams.size(); ++i) {
            basisLZ[i].imageFlags.resize(maxImageCount);
            basisLZ[i].rgbSliceByteOffset.resize(maxImageCount);
            basisLZ[i].rgbSliceByteLength.resize(maxImageCount);
            basisLZ[i].alphaSliceByteOffset.resize(maxImageCount);
            basisLZ[i].alphaSliceByteLength.resize(maxImageCount);
        }

        for (std::size_t imageIndex = 0; imageIndex < maxImageCount; ++imageIndex) {
            diff << DiffFlags(fmt::format("Image{}.imageFlags", imageIndex),
                fmt::format("/supercompressionGlobalData/images/{}/imageFlags", imageIndex),
                basisLZ[0].imageFlags[imageIndex], basisLZ[1].imageFlags[imageIndex], ktxBUImageFlagsBitString);

            diff << Diff(fmt::format("Image{}.rgbSliceByteLength", imageIndex),
                fmt::format("/supercompressionGlobalData/images/{}/rgbSliceByteLength", imageIndex),
                basisLZ[0].rgbSliceByteLength[imageIndex], basisLZ[1].rgbSliceByteLength[imageIndex]);

            diff << Diff(fmt::format("Image{}.rgbSliceByteOffset", imageIndex),
                fmt::format("/supercompressionGlobalData/images/{}/rgbSliceByteOffset", imageIndex),
                basisLZ[0].rgbSliceByteOffset[imageIndex], basisLZ[1].rgbSliceByteOffset[imageIndex]);

            diff << Diff(fmt::format("Image{}.alphaSliceByteLength", imageIndex),
                fmt::format("/supercompressionGlobalData/images/{}/alphaSliceByteLength", imageIndex),
                basisLZ[0].alphaSliceByteLength[imageIndex], basisLZ[1].alphaSliceByteLength[imageIndex]);

            diff << Diff(fmt::format("Image{}.alphaSliceByteOffset", imageIndex),
                fmt::format("/supercompressionGlobalData/images/{}/alphaSliceByteOffset", imageIndex),
                basisLZ[0].alphaSliceByteOffset[imageIndex], basisLZ[1].alphaSliceByteOffset[imageIndex]);
        }

        if (options.ignoreSGD != IgnoreSGD::payload) {
            compareSGDPayload("endpointsData", "/supercompressionGlobalData/endpointsData",
                basisLZ[0].endpointsByteOffset, basisLZ[0].endpointsByteLength,
                basisLZ[1].endpointsByteOffset, basisLZ[1].endpointsByteLength);
            compareSGDPayload("selectorsData", "/supercompressionGlobalData/selectorsData",
                basisLZ[0].selectorsByteOffset, basisLZ[0].selectorsByteLength,
                basisLZ[1].selectorsByteOffset, basisLZ[1].selectorsByteLength);
            compareSGDPayload("tablesData", "/supercompressionGlobalData/tablesData",
                basisLZ[0].tablesByteOffset, basisLZ[0].tablesByteLength,
                basisLZ[1].tablesByteOffset, basisLZ[1].tablesByteLength);
            compareSGDPayload("extendedData", "/supercompressionGlobalData/extendedData",
                basisLZ[0].extendedByteOffset, basisLZ[0].extendedByteLength,
                basisLZ[1].extendedByteOffset, basisLZ[1].extendedByteLength);
        }
    } else if (options.ignoreSGD == IgnoreSGD::none) {
        diff.setContext("Unrecognized Supercompression Global Data\n\n");

        // Just compare raw payloads of the SGDs
        compareSGDPayload("SGD", "/supercompressionGlobalData/rawPayload",
            0, headers[0].supercompressionGlobalData.byteLength,
            0, headers[1].supercompressionGlobalData.byteLength);
    }
}

} // namespace ktx

KTX_COMMAND_ENTRY_POINT(ktxCompare, ktx::CommandCompare)
