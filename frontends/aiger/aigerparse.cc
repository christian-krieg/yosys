/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *                      Eddie Hung <eddie@fpgeh.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

// [[CITE]] The AIGER And-Inverter Graph (AIG) Format Version 20071012
// Armin Biere. The AIGER And-Inverter Graph (AIG) Format Version 20071012. Technical Report 07/1, October 2011, FMV Reports Series, Institute for Formal Models and Verification, Johannes Kepler University, Altenbergerstr. 69, 4040 Linz, Austria.
// http://fmv.jku.at/papers/Biere-FMV-TR-07-1.pdf

#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/consteval.h"
#include "aigerparse.h"

#include <boost/endian/buffers.hpp>
#include <boost/lexical_cast.hpp>

YOSYS_NAMESPACE_BEGIN

#define log_debug log
#define log_debug(...) ;

AigerReader::AigerReader(RTLIL::Design *design, std::istream &f, RTLIL::IdString module_name, RTLIL::IdString clk_name, std::string map_filename, bool wideports)
    : design(design), f(f), clk_name(clk_name), map_filename(map_filename), wideports(wideports)
{
    module = new RTLIL::Module;
    module->name = module_name;
    if (design->module(module->name))
        log_error("Duplicate definition of module %s!\n", log_id(module->name));
}

void AigerReader::parse_aiger()
{
    std::string header;
    f >> header;
    if (header != "aag" && header != "aig")
        log_error("Unsupported AIGER file!\n");

    // Parse rest of header
    if (!(f >> M >> I >> L >> O >> A))
        log_error("Invalid AIGER header\n");

    // Optional values
    B = C = J = F = 0;
    for (auto &i : std::array<std::reference_wrapper<unsigned>,4>{B, C, J, F}) {
        if (f.peek() != ' ') break;
        if (!(f >> i))
            log_error("Invalid AIGER header\n");
    }

    std::string line;
    std::getline(f, line); // Ignore up to start of next line, as standard
                           // says anything that follows could be used for
                           // optional sections

    log_debug("M=%u I=%u L=%u O=%u A=%u B=%u C=%u J=%u F=%u\n", M, I, L, O, A, B, C, J, F);

    line_count = 1;

    if (header == "aag")
        parse_aiger_ascii();
    else if (header == "aig")
        parse_aiger_binary();
    else
        log_abort();

    // Parse footer (symbol table, comments, etc.)
    unsigned l1;
    std::string s;
    for (int c = f.peek(); c != EOF; c = f.peek(), ++line_count) {
        if (c == 'i' || c == 'l' || c == 'o') {
            f.ignore(1);
            if (!(f >> l1 >> s))
                log_error("Line %u cannot be interpreted as a symbol entry!\n", line_count);

            if ((c == 'i' && l1 > inputs.size()) || (c == 'l' && l1 > latches.size()) || (c == 'o' && l1 > outputs.size()))
                log_error("Line %u has invalid symbol position!\n", line_count);

            RTLIL::Wire* wire;
            if (c == 'i') wire = inputs[l1];
            else if (c == 'l') wire = latches[l1];
            else if (c == 'o') wire = outputs[l1];
            else log_abort();

            module->rename(wire, stringf("\\%s", s.c_str()));
        }
        else if (c == 'b' || c == 'j' || c == 'f') {
            // TODO
        }
        else if (c == 'c') {
            f.ignore(1);
            if (f.peek() == '\n')
                break;
            // Else constraint (TODO)
        }
        else
            log_error("Line %u: cannot interpret first character '%c'!\n", line_count, c);
        std::getline(f, line); // Ignore up to start of next line
    }

    dict<RTLIL::IdString, int> wideports_cache;

    if (!map_filename.empty()) {
        std::ifstream mf(map_filename);
        std::string type, symbol;
        int variable, index;
        while (mf >> type >> variable >> index >> symbol) {
            RTLIL::IdString escaped_symbol = RTLIL::escape_id(symbol);
            if (type == "input") {
                log_assert(static_cast<unsigned>(variable) < inputs.size());
                RTLIL::Wire* wire = inputs[variable];
                log_assert(wire);
                log_assert(wire->port_input);

                if (index == 0)
                    module->rename(wire, RTLIL::escape_id(symbol));
                else if (index > 0) {
                    module->rename(wire, RTLIL::escape_id(stringf("%s[%d]", symbol.c_str(), index)));
                    if (wideports)
                        wideports_cache[escaped_symbol] = std::max(wideports_cache[escaped_symbol], index);
                }
            }
            else if (type == "output") {
                log_assert(static_cast<unsigned>(variable) < outputs.size());
                RTLIL::Wire* wire = outputs[variable];
                log_assert(wire);
                log_assert(wire->port_output);

                if (index == 0)
                    module->rename(wire, RTLIL::escape_id(symbol));
                else if (index > 0) {
                    module->rename(wire, RTLIL::escape_id(stringf("%s[%d]", symbol.c_str(), index)));
                    if (wideports)
                        wideports_cache[escaped_symbol] = std::max(wideports_cache[escaped_symbol], index);
                }
            }
            else
                log_error("Symbol type '%s' not recognised.\n", type.c_str());
        }
    }

    for (auto &wp : wideports_cache) {
        auto name = wp.first;
        int width = wp.second + 1;

        RTLIL::Wire *wire = module->wire(name);
        if (wire)
            module->rename(wire, RTLIL::escape_id(stringf("%s[%d]", name.c_str(), 0)));

        // Do not make ports with a mix of input/output into
        // wide ports
        bool port_input = false, port_output = false;
        for (int i = 0; i < width; i++) {
            RTLIL::IdString other_name = name.str() + stringf("[%d]", i);
            RTLIL::Wire *other_wire = module->wire(other_name);
            if (other_wire) {
                port_input = port_input || other_wire->port_input;
                port_output = port_output || other_wire->port_output;
            }
        }
        if ((port_input && port_output) || (!port_input && !port_output))
            continue;

        wire = module->addWire(name, width);
        wire->port_input = port_input;
        wire->port_output = port_output;

        for (int i = 0; i < width; i++) {
            RTLIL::IdString other_name = name.str() + stringf("[%d]", i);
            RTLIL::Wire *other_wire = module->wire(other_name);
            if (other_wire) {
                other_wire->port_input = false;
                other_wire->port_output = false;
                if (wire->port_input)
                    module->connect(other_wire, SigSpec(wire, i));
                else
                    module->connect(SigSpec(wire, i), other_wire);
            }
        }
    }

    module->fixup_ports();
    design->add(module);

    Pass::call(design, "clean");
}

static uint32_t parse_xaiger_literal(std::istream &f)
{
    boost::endian::big_uint32_buf_t l;
    f.read(reinterpret_cast<char*>(&l), sizeof(l));
    if (f.gcount() != sizeof(l))
        log_error("Offset %ld: unable to read literal!\n", boost::lexical_cast<int64_t>(f.tellg()));
    return l.value();
}

static RTLIL::Wire* createWireIfNotExists(RTLIL::Module *module, unsigned literal)
{
    const unsigned variable = literal >> 1;
    const bool invert = literal & 1;
    RTLIL::IdString wire_name(stringf("\\n%d%s", variable, invert ? "_inv" : "")); // FIXME: is "_inv" the right suffix?
    RTLIL::Wire *wire = module->wire(wire_name);
    if (wire) return wire;
    log_debug("Creating %s\n", wire_name.c_str());
    wire = module->addWire(wire_name);
    wire->port_input = wire->port_output = false;
    if (!invert) return wire;
    RTLIL::IdString wire_inv_name(stringf("\\n%d", variable));
    RTLIL::Wire *wire_inv = module->wire(wire_inv_name);
    if (wire_inv) {
        if (module->cell(wire_inv_name)) return wire;
    }
    else {
        log_debug("Creating %s\n", wire_inv_name.c_str());
        wire_inv = module->addWire(wire_inv_name);
        wire_inv->port_input = wire_inv->port_output = false;
    }

    log_debug("Creating %s = ~%s\n", wire_name.c_str(), wire_inv_name.c_str());
    module->addNotGate(stringf("\\n%d_not", variable), wire_inv, wire); // FIXME: is "_not" the right suffix?

    return wire;
}

void AigerReader::parse_xaiger()
{
    std::string header;
    f >> header;
    if (header != "aag" && header != "aig")
        log_error("Unsupported AIGER file!\n");

    // Parse rest of header
    if (!(f >> M >> I >> L >> O >> A))
        log_error("Invalid AIGER header\n");

    // Optional values
    B = C = J = F = 0;

    std::string line;
    std::getline(f, line); // Ignore up to start of next line, as standard
                           // says anything that follows could be used for
                           // optional sections

    log_debug("M=%u I=%u L=%u O=%u A=%u\n", M, I, L, O, A);

    line_count = 1;

    if (header == "aag")
        parse_aiger_ascii();
    else if (header == "aig")
        parse_aiger_binary();
    else
        log_abort();

    // Parse footer (symbol table, comments, etc.)
    unsigned l1;
    std::string s;
    bool comment_seen = false;
    for (int c = f.peek(); c != EOF; c = f.peek()) {
        if (comment_seen || c == 'c') {
            if (!comment_seen) {
                f.ignore(1);
                c = f.peek();
                comment_seen = true;
            }
            if (c == '\n')
                break;
            f.ignore(1);
            // XAIGER extensions
            if (c == 'm') {
                uint32_t dataSize = parse_xaiger_literal(f);
                uint32_t lutNum = parse_xaiger_literal(f);
                uint32_t lutSize = parse_xaiger_literal(f);
                log_debug("m: dataSize=%u lutNum=%u lutSize=%u\n", dataSize, lutNum, lutSize);
                for (unsigned i = 0; i < lutNum; ++i) {
                    uint32_t rootNodeID = parse_xaiger_literal(f);
                    uint32_t cutLeavesM = parse_xaiger_literal(f);
                    log_debug("rootNodeID=%d cutLeavesM=%d\n", rootNodeID, cutLeavesM);
                    RTLIL::Wire *output_sig = module->wire(stringf("\\n%d", rootNodeID));
                    uint32_t nodeID;
                    RTLIL::SigSpec input_sig;
                    for (unsigned j = 0; j < cutLeavesM; ++j) {
                        nodeID = parse_xaiger_literal(f);
                        log_debug("\t%u\n", nodeID);
                        RTLIL::Wire *wire = module->wire(stringf("\\n%d", nodeID));
                        log_assert(wire);
                        input_sig.append(wire);
                    }
                    RTLIL::Const lut_mask(RTLIL::State::Sx, 1 << input_sig.size());
                    ConstEval ce(module);
                    for (int j = 0; j < (1 << cutLeavesM); ++j) {
                        ce.push();
                        ce.set(input_sig, RTLIL::Const{j, static_cast<int>(cutLeavesM)});
                        RTLIL::SigSpec o(output_sig);
                        ce.eval(o);
                        lut_mask[j] = o.as_const()[0];
                        ce.pop();
                    }
                    RTLIL::Cell *output_cell = module->cell(stringf("\\n%d_and", rootNodeID));
                    log_assert(output_cell);
                    module->remove(output_cell);
					module->addLut(NEW_ID, input_sig, output_sig, std::move(lut_mask));
                }
            }
            else if (c == 'n') {
               parse_xaiger_literal(f);
               f >> s;
               log_debug("n: '%s'\n", s.c_str());
            }
        }
        else if (c == 'i' || c == 'l' || c == 'o') {
            f.ignore(1);
            if (!(f >> l1 >> s))
                log_error("Line %u cannot be interpreted as a symbol entry!\n", line_count);

            if ((c == 'i' && l1 > inputs.size()) || (c == 'l' && l1 > latches.size()) || (c == 'o' && l1 > outputs.size()))
                log_error("Line %u has invalid symbol position!\n", line_count);

            RTLIL::Wire* wire;
            if (c == 'i') wire = inputs[l1];
            else if (c == 'l') wire = latches[l1];
            else if (c == 'o') wire = outputs[l1];
            else log_abort();

            module->rename(wire, stringf("\\%s", s.c_str()));
            std::getline(f, line); // Ignore up to start of next line
            ++line_count;
        }
        else
            log_error("Line %u: cannot interpret first character '%c'!\n", line_count, c);
    }

    dict<RTLIL::IdString, int> wideports_cache;

    if (!map_filename.empty()) {
        std::ifstream mf(map_filename);
        std::string type, symbol;
        int variable, index;
        while (mf >> type >> variable >> index >> symbol) {
            RTLIL::IdString escaped_symbol = RTLIL::escape_id(symbol);
            if (type == "input") {
                log_assert(static_cast<unsigned>(variable) < inputs.size());
                RTLIL::Wire* wire = inputs[variable];
                log_assert(wire);
                log_assert(wire->port_input);

                if (index == 0)
                    module->rename(wire, RTLIL::escape_id(symbol));
                else if (index > 0) {
                    module->rename(wire, RTLIL::escape_id(stringf("%s[%d]", symbol.c_str(), index)));
                    if (wideports)
                        wideports_cache[escaped_symbol] = std::max(wideports_cache[escaped_symbol], index);
                }
            }
            else if (type == "output") {
                log_assert(static_cast<unsigned>(variable) < outputs.size());
                RTLIL::Wire* wire = outputs[variable];
                log_assert(wire);
                log_assert(wire->port_output);

                if (index == 0)
                    module->rename(wire, RTLIL::escape_id(symbol));
                else if (index > 0) {
                    module->rename(wire, RTLIL::escape_id(stringf("%s[%d]", symbol.c_str(), index)));
                    if (wideports)
                        wideports_cache[escaped_symbol] = std::max(wideports_cache[escaped_symbol], index);
                }
            }
            else
                log_error("Symbol type '%s' not recognised.\n", type.c_str());
        }
    }

    for (auto &wp : wideports_cache) {
        auto name = wp.first;
        int width = wp.second + 1;

        RTLIL::Wire *wire = module->wire(name);
        if (wire)
            module->rename(wire, RTLIL::escape_id(stringf("%s[%d]", name.c_str(), 0)));

        // Do not make ports with a mix of input/output into
        // wide ports
        bool port_input = false, port_output = false;
        for (int i = 0; i < width; i++) {
            RTLIL::IdString other_name = name.str() + stringf("[%d]", i);
            RTLIL::Wire *other_wire = module->wire(other_name);
            if (other_wire) {
                port_input = port_input || other_wire->port_input;
                port_output = port_output || other_wire->port_output;
            }
        }
        if ((port_input && port_output) || (!port_input && !port_output))
            continue;

        wire = module->addWire(name, width);
        wire->port_input = port_input;
        wire->port_output = port_output;

        for (int i = 0; i < width; i++) {
            RTLIL::IdString other_name = name.str() + stringf("[%d]", i);
            RTLIL::Wire *other_wire = module->wire(other_name);
            if (other_wire) {
                other_wire->port_input = false;
                other_wire->port_output = false;
                if (wire->port_input)
                    module->connect(other_wire, SigSpec(wire, i));
                else
                    module->connect(SigSpec(wire, i), other_wire);
            }
        }
    }

    module->fixup_ports();
    design->add(module);

    Pass::call(design, "clean");
}

void AigerReader::parse_aiger_ascii()
{
    std::string line;
    std::stringstream ss;

    unsigned l1, l2, l3;

    // Parse inputs
    for (unsigned i = 0; i < I; ++i, ++line_count) {
        if (!(f >> l1))
            log_error("Line %u cannot be interpreted as an input!\n", line_count);
        log_debug("%d is an input\n", l1);
        log_assert(!(l1 & 1)); // TODO: Inputs can't be inverted?
        RTLIL::Wire *wire = createWireIfNotExists(module, l1);
        wire->port_input = true;
        inputs.push_back(wire);
    }

    // Parse latches
    RTLIL::Wire *clk_wire = nullptr;
    if (L > 0) {
        clk_wire = module->wire(clk_name);
        log_assert(!clk_wire);
        log_debug("Creating %s\n", clk_name.c_str());
        clk_wire = module->addWire(clk_name);
        clk_wire->port_input = true;
        clk_wire->port_output = false;
    }
    for (unsigned i = 0; i < L; ++i, ++line_count) {
        if (!(f >> l1 >> l2))
            log_error("Line %u cannot be interpreted as a latch!\n", line_count);
        log_debug("%d %d is a latch\n", l1, l2);
        log_assert(!(l1 & 1)); // TODO: Latch outputs can't be inverted?
        RTLIL::Wire *q_wire = createWireIfNotExists(module, l1);
        RTLIL::Wire *d_wire = createWireIfNotExists(module, l2);

        module->addDffGate(NEW_ID, clk_wire, d_wire, q_wire);

        // Reset logic is optional in AIGER 1.9
        if (f.peek() == ' ') {
            if (!(f >> l3))
                log_error("Line %u cannot be interpreted as a latch!\n", line_count);

            if (l3 == 0 || l3 == 1)
                q_wire->attributes["\\init"] = RTLIL::Const(l3);
            else if (l3 == l1) {
                //q_wire->attributes["\\init"] = RTLIL::Const(RTLIL::State::Sx);
            }
            else
                log_error("Line %u has invalid reset literal for latch!\n", line_count);
        }
        else {
            // AIGER latches are assumed to be initialized to zero
            q_wire->attributes["\\init"] = RTLIL::Const(0);
        }
        latches.push_back(q_wire);
    }

    // Parse outputs
    for (unsigned i = 0; i < O; ++i, ++line_count) {
        if (!(f >> l1))
            log_error("Line %u cannot be interpreted as an output!\n", line_count);

        RTLIL::Wire *wire;
        if (l1 == 0 || l1 == 1) {
            wire = module->addWire(stringf("\\o%zu", outputs.size()));
            if (l1 == 0)
                module->connect(wire, RTLIL::State::S0);
            else if (l1 == 1)
                module->connect(wire, RTLIL::State::S1);
            else
                log_abort();
        }
        else {
            log_debug("%d is an output\n", l1);
            wire = createWireIfNotExists(module, l1);
        }
        wire->port_output = true;
        log_assert(!wire->port_input);
        outputs.push_back(wire);
    }
    std::getline(f, line); // Ignore up to start of next line

    // TODO: Parse bad state properties
    for (unsigned i = 0; i < B; ++i, ++line_count)
        std::getline(f, line); // Ignore up to start of next line

    // TODO: Parse invariant constraints
    for (unsigned i = 0; i < C; ++i, ++line_count)
        std::getline(f, line); // Ignore up to start of next line

    // TODO: Parse justice properties
    for (unsigned i = 0; i < J; ++i, ++line_count)
        std::getline(f, line); // Ignore up to start of next line

    // TODO: Parse fairness constraints
    for (unsigned i = 0; i < F; ++i, ++line_count)
        std::getline(f, line); // Ignore up to start of next line

    // Parse AND
    for (unsigned i = 0; i < A; ++i) {
        if (!(f >> l1 >> l2 >> l3))
            log_error("Line %u cannot be interpreted as an AND!\n", line_count);

        log_debug("%d %d %d is an AND\n", l1, l2, l3);
        log_assert(!(l1 & 1));
        RTLIL::Wire *o_wire = createWireIfNotExists(module, l1);
        RTLIL::Wire *i1_wire = createWireIfNotExists(module, l2);
        RTLIL::Wire *i2_wire = createWireIfNotExists(module, l3);
        module->addAndGate(o_wire->name.str() + "_and", i1_wire, i2_wire, o_wire);
    }
    std::getline(f, line);
}

static unsigned parse_next_delta_literal(std::istream &f, unsigned ref)
{
    unsigned x = 0, i = 0;
    unsigned char ch;
    while ((ch = f.get()) & 0x80)
        x |= (ch & 0x7f) << (7 * i++);
    return ref - (x | (ch << (7 * i)));
}

void AigerReader::parse_aiger_binary()
{
    unsigned l1, l2, l3;
    std::string line;

    // Parse inputs
    for (unsigned i = 1; i <= I; ++i) {
        log_debug("%d is an input\n", i);
        RTLIL::Wire *wire = createWireIfNotExists(module, i << 1);
        wire->port_input = true;
        log_assert(!wire->port_output);
        inputs.push_back(wire);
    }

    // Parse latches
    RTLIL::Wire *clk_wire = nullptr;
    if (L > 0) {
        clk_wire = module->wire(clk_name);
        log_assert(!clk_wire);
        log_debug("Creating %s\n", clk_name.c_str());
        clk_wire = module->addWire(clk_name);
        clk_wire->port_input = true;
        clk_wire->port_output = false;
    }
    l1 = (I+1) * 2;
    for (unsigned i = 0; i < L; ++i, ++line_count, l1 += 2) {
        if (!(f >> l2))
            log_error("Line %u cannot be interpreted as a latch!\n", line_count);
        log_debug("%d %d is a latch\n", l1, l2);
        RTLIL::Wire *q_wire = createWireIfNotExists(module, l1);
        RTLIL::Wire *d_wire = createWireIfNotExists(module, l2);

        module->addDff(NEW_ID, clk_wire, d_wire, q_wire);

        // Reset logic is optional in AIGER 1.9
        if (f.peek() == ' ') {
            if (!(f >> l3))
                log_error("Line %u cannot be interpreted as a latch!\n", line_count);

            if (l3 == 0 || l3 == 1)
                q_wire->attributes["\\init"] = RTLIL::Const(l3);
            else if (l3 == l1) {
                //q_wire->attributes["\\init"] = RTLIL::Const(RTLIL::State::Sx);
            }
            else
                log_error("Line %u has invalid reset literal for latch!\n", line_count);
        }
        else {
            // AIGER latches are assumed to be initialized to zero
            q_wire->attributes["\\init"] = RTLIL::Const(0);
        }
        latches.push_back(q_wire);
    }

    // Parse outputs
    for (unsigned i = 0; i < O; ++i, ++line_count) {
        if (!(f >> l1))
            log_error("Line %u cannot be interpreted as an output!\n", line_count);

        RTLIL::Wire *wire;
        if (l1 == 0 || l1 == 1) {
            wire = module->addWire(stringf("\\o%zu", outputs.size()));
            if (l1 == 0)
                module->connect(wire, RTLIL::State::S0);
            else if (l1 == 1)
                module->connect(wire, RTLIL::State::S1);
            else
                log_abort();
        }
        else {
            log_debug("%d is an output\n", l1);
            wire = createWireIfNotExists(module, l1);
        }
        log_assert(!wire->port_input);
        wire->port_output = true;
        outputs.push_back(wire);
    }
    std::getline(f, line); // Ignore up to start of next line

    // TODO: Parse bad state properties
    for (unsigned i = 0; i < B; ++i, ++line_count)
        std::getline(f, line); // Ignore up to start of next line

    // TODO: Parse invariant constraints
    for (unsigned i = 0; i < C; ++i, ++line_count)
        std::getline(f, line); // Ignore up to start of next line

    // TODO: Parse justice properties
    for (unsigned i = 0; i < J; ++i, ++line_count)
        std::getline(f, line); // Ignore up to start of next line

    // TODO: Parse fairness constraints
    for (unsigned i = 0; i < F; ++i, ++line_count)
        std::getline(f, line); // Ignore up to start of next line

    // Parse AND
    l1 = (I+L+1) << 1;
    for (unsigned i = 0; i < A; ++i, ++line_count, l1 += 2) {
        l2 = parse_next_delta_literal(f, l1);
        l3 = parse_next_delta_literal(f, l2);

        log_debug("%d %d %d is an AND\n", l1, l2, l3);
        log_assert(!(l1 & 1));
        RTLIL::Wire *o_wire = createWireIfNotExists(module, l1);
        RTLIL::Wire *i1_wire = createWireIfNotExists(module, l2);
        RTLIL::Wire *i2_wire = createWireIfNotExists(module, l3);
        module->addAndGate(o_wire->name.str() + "_and", i1_wire, i2_wire, o_wire);
    }
}

struct AigerFrontend : public Frontend {
    AigerFrontend() : Frontend("aiger", "read AIGER file") { }
    void help() YS_OVERRIDE
    {
        //   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
        log("\n");
        log("    read_aiger [options] [filename]\n");
        log("\n");
        log("Load module from an AIGER file into the current design.\n");
        log("\n");
        log("    -module_name <module_name>\n");
        log("        Name of module to be created (default: <filename>)"
#ifdef _WIN32
		                                                   "top" // FIXME
#else
		                                                   "<filename>"
#endif
                                                           ")\n");
        log("\n");
        log("    -clk_name <wire_name>\n");
        log("        AIGER latches to be transformed into posedge DFFs clocked by wire of");
        log("        this name (default: clk)\n");
        log("\n");
		log("    -map <filename>\n");
		log("        read file with port and latch symbols\n");
        log("\n");
		log("    -wideports\n");
		log("        Merge ports that match the pattern 'name[int]' into a single\n");
		log("        multi-bit port 'name'.\n");
		log("\n");
    }
    void execute(std::istream *&f, std::string filename, std::vector<std::string> args, RTLIL::Design *design) YS_OVERRIDE
    {
        log_header(design, "Executing AIGER frontend.\n");

        RTLIL::IdString clk_name = "\\clk";
        RTLIL::IdString module_name;
        std::string map_filename;
        bool wideports = false;

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			std::string arg = args[argidx];
			if (arg == "-module_name" && argidx+1 < args.size()) {
				module_name = RTLIL::escape_id(args[++argidx]);
				continue;
			}
			if (arg == "-clk_name" && argidx+1 < args.size()) {
				clk_name = RTLIL::escape_id(args[++argidx]);
				continue;
			}
			if (map_filename.empty() && arg == "-map" && argidx+1 < args.size()) {
				map_filename = args[++argidx];
				continue;
			}
			if (arg == "-wideports") {
				wideports = true;
				continue;
			}
			break;
		}
		extra_args(f, filename, args, argidx);

        if (module_name.empty()) {
#ifdef _WIN32
            module_name = "top"; // FIXME: basename equivalent on Win32?
#else
            module_name = RTLIL::escape_id(basename(filename.c_str()));
#endif
        }

        AigerReader reader(design, *f, module_name, clk_name, map_filename, wideports);
		reader.parse_aiger();
    }
} AigerFrontend;

YOSYS_NAMESPACE_END