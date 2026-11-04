using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;

namespace Translator.Core.Analysis;

/// <summary>
/// Proves which nonvolatile GPRs (r1, r2, r13-r31) a translated function hands back with exactly the value it
/// received, on every path that returns. A caller can then keep such a register in its own local across the
/// call instead of reloading it from <c>CpuContext</c>: the callee's prologue spill and epilogue restore leave
/// the architectural value unchanged, so the reload only re-reads what the caller flushed.
///
/// The proof is a forward dataflow over architectural register names (SSA versions are ignored because a call
/// changes registers without creating new versions). Values are tracked symbolically as "the entry value of rK"
/// or "entry r1 + delta", and 4-byte slots of the function's own frame remember which entry value was stored
/// there, so the canonical <c>stw rK, n(r1)</c> ... <c>lwz rK, n(r1)</c> spill, <c>stmw</c>/<c>lmw</c> and the
/// CodeWarrior <c>_save_gpr_N</c>/<c>_rest_gpr_N</c> thunks all prove preservation.
///
/// Conservative by construction: a call clobbers every GPR its contract may write (all of them for an indirect
/// call, a full-fence callee or a complete-boundary helper) except what this analysis already proved the callee
/// preserves, so the fixpoint grows from "nothing preserved" and every intermediate result is sound. Indirect
/// jumps, branches out of the function and anything not modelled prove nothing. Two assumptions, both ones the
/// guest code itself relies on:
/// <list type="bullet">
/// <item>A call that returns hands back the caller's r1. Every PowerPC caller addresses its own frame through r1
/// right after its calls; the context-switching OS routines that load a different r1 never return to their
/// call site (OSLoadContext is a [[noreturn]] native).</item>
/// <item>As with the existing stack-slot passes, a store through a pointer that is not derived from r1 does not
/// hit the function's own register save slots, which no guest code takes the address of.</item>
/// </list>
/// </summary>
public static class GuestNonvolatileGprPreservation
{
    /// <summary>r1, r2 and r13-r31: the registers the PowerPC EABI requires a callee to preserve.</summary>
    public const uint CandidateMask = (1u << 1) | (1u << 2) | 0xFFFF_E000u;

    private const int MaxRounds = 64;

    /// <param name="contracts">Final call contracts (natives included). A call clobbers what its contract may
    /// write, exactly the set the code generator reloads after it, minus what this analysis proved preserved;
    /// a missing contract or a full-synchronization fence clobbers every GPR.</param>
    public static Dictionary<uint, uint> Analyze(
        IReadOnlyDictionary<uint, IrFunction> functions,
        IReadOnlySet<uint> excludedTargets,
        IReadOnlyDictionary<uint, GuestAbiContract> contracts)
    {
        var masks = new Dictionary<uint, uint>(functions.Count);
        foreach (var address in functions.Keys) masks[address] = 0;
        var order = functions.Keys.OrderBy(static address => address).ToArray();

        for (var round = 0; round < MaxRounds; ++round)
        {
            var changed = false;
            foreach (var address in order)
            {
                if (excludedTargets.Contains(address)) continue;
                var mask = AnalyzeFunction(functions[address], target =>
                {
                    if (!contracts.TryGetValue(target, out var contract) || contract.HasFullSynchronizationFence)
                        return uint.MaxValue;
                    var preserved = !excludedTargets.Contains(target) && masks.TryGetValue(target, out var known) ? known : 0u;
                    return contract.GprPossibleWriteMask & ~preserved;
                });
                // Monotone: callee facts only grow, so a function's proof only grows with them.
                mask |= masks[address];
                if (mask != masks[address])
                {
                    masks[address] = mask;
                    changed = true;
                }
            }
            if (!changed) break;
        }
        return masks;
    }

    private enum SymKind : byte { Unknown, Entry, Frame }

    private readonly record struct Sym(SymKind Kind, int Value)
    {
        public static readonly Sym Unknown = new(SymKind.Unknown, 0);
        public static Sym EntryOf(int register) => register == 1 ? new Sym(SymKind.Frame, 0) : new Sym(SymKind.Entry, register);
        public static Sym Frame(int delta) => new(SymKind.Frame, delta);
    }

    private sealed class State
    {
        public readonly Dictionary<string, Sym> Names = new(StringComparer.OrdinalIgnoreCase);
        public readonly Dictionary<int, Sym> Slots = new();

        public State Clone()
        {
            var copy = new State();
            foreach (var pair in Names) copy.Names[pair.Key] = pair.Value;
            foreach (var pair in Slots) copy.Slots[pair.Key] = pair.Value;
            return copy;
        }

        /// <summary>Meet into this state; returns true when it changed. Missing entries are Unknown.</summary>
        public bool MeetWith(State other)
        {
            var changed = false;
            foreach (var key in Names.Keys.ToArray())
            {
                if (Names[key].Kind == SymKind.Unknown) continue;
                if (!other.Names.TryGetValue(key, out var value) || value != Names[key])
                {
                    Names[key] = Sym.Unknown;
                    changed = true;
                }
            }
            foreach (var key in Slots.Keys.ToArray())
            {
                if (!other.Slots.TryGetValue(key, out var value) || value != Slots[key])
                {
                    Slots.Remove(key);
                    changed = true;
                }
            }
            return changed;
        }

        public Sym Get(string name) => Names.TryGetValue(name, out var value) ? value : Sym.Unknown;

        /// <summary>Clobbers the masked GPRs across a call; r1 survives every call that returns.</summary>
        public void KillAcrossCall(uint mask)
        {
            mask &= ~(1u << 1);
            for (var register = 0; register < 32; ++register)
                if ((mask & (1u << register)) != 0) Names[$"r{register}"] = Sym.Unknown;
        }

        public void KillSlots(int start, int size)
        {
            foreach (var key in Slots.Keys.ToArray())
                if (key < start + size && start < key + 4) Slots.Remove(key);
        }
    }

    private static uint AnalyzeFunction(IrFunction function, Func<uint, uint> calleeClobbers)
    {
        var cfg = IrCfg.Build(function);
        var blocks = cfg.Blocks;
        if (!blocks.TryGetValue(function.EntryLabel, out var entryBlock)) return 0;
        var entryLabel = entryBlock.Label;

        var entry = new State();
        for (var register = 0; register < 32; ++register)
            entry.Names[$"r{register}"] = (CandidateMask & (1u << register)) != 0 ? Sym.EntryOf(register) : Sym.Unknown;

        var blockIn = new Dictionary<string, State>(StringComparer.OrdinalIgnoreCase) { [entryLabel] = entry };
        var work = new Queue<string>();
        var queued = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { entryLabel };
        work.Enqueue(entryLabel);
        var preserved = CandidateMask;
        var sawReturn = false;
        var visits = 0;

        while (work.Count != 0)
        {
            if (++visits > 20_000) return 0;
            var label = work.Dequeue();
            queued.Remove(label);
            var instructions = blocks[label].Instructions;
            var state = blockIn[label].Clone();

            // Same control-flow semantics as IrCfg, which the code generator follows: only a block's last
            // instruction transfers control, and any other block falls through to the next one.
            for (var index = 0; index < instructions.Count; ++index)
            {
                var instruction = instructions[index];
                var isLast = index == instructions.Count - 1;
                switch (instruction)
                {
                    case IrBranch branch:
                        if (!isLast) return 0;
                        if (!blocks.ContainsKey(branch.TrueLabel) || !blocks.ContainsKey(branch.FalseLabel))
                            return 0;
                        break;
                    case IrJump jump:
                        if (!isLast) return 0;
                        if (!blocks.ContainsKey(jump.TargetLabel)) return 0;
                        break;
                    case IrJumpTable table:
                        if (!isLast) return 0;
                        if (table.Cases.Any(c => !blocks.ContainsKey(c.TargetLabel))) return 0;
                        break;
                    case IrReturn:
                        if (!isLast) return 0;
                        sawReturn = true;
                        preserved &= ProvenAtReturn(state);
                        break;
                    case IrIndirectJump:
                    case IrUndefined:
                        // Leaves the function (tail call through CTR) or traps: nothing provable.
                        return 0;
                    default:
                        if (!Transfer(instruction, state, calleeClobbers)) return 0;
                        break;
                }
            }

            foreach (var successor in cfg.Successors(label))
            {
                if (!blockIn.TryGetValue(successor, out var existing))
                {
                    blockIn[successor] = state.Clone();
                }
                else if (!existing.MeetWith(state))
                {
                    continue;
                }
                if (queued.Add(successor)) work.Enqueue(successor);
            }
        }

        return sawReturn ? preserved : 0;
    }

    private static uint ProvenAtReturn(State state)
    {
        uint mask = 0;
        for (var register = 0; register < 32; ++register)
        {
            if ((CandidateMask & (1u << register)) == 0) continue;
            if (state.Get($"r{register}") == Sym.EntryOf(register)) mask |= 1u << register;
        }
        return mask;
    }

    private static string Name(string? register) =>
        string.IsNullOrEmpty(register) ? string.Empty : RegisterNameUtils.StripNumericSuffix(register);

    private static Sym Eval(IrValue value, State state) =>
        value.Kind == "register" ? state.Get(Name(value.RegisterName)) : Sym.Unknown;

    private static bool IsConstant(IrValue value, out long constant)
    {
        constant = value.Constant ?? 0;
        return value.Kind != "register" && value.Constant.HasValue;
    }

    private static void Define(State state, string? destination, Sym value)
    {
        var name = Name(destination);
        if (name.Length != 0) state.Names[name] = value;
    }

    private static bool TrySlot(IrAddress address, State state, out int slot)
    {
        var baseValue = state.Get(Name(address.Base));
        slot = baseValue.Kind == SymKind.Frame ? baseValue.Value + address.Offset : 0;
        return baseValue.Kind == SymKind.Frame;
    }

    /// <summary>Applies one non-terminator instruction. False means "give up on this function".</summary>
    private static bool Transfer(IrInstruction instruction, State state, Func<uint, uint> calleeClobbers)
    {
        switch (instruction)
        {
            case IrComment:
            case IrTracePpc:
            case IrSetCrField:
                return true;

            case IrPhi phi:
                // Registers are tracked by architectural name, so the block-entry meet already is the phi.
                if (!Name(phi.Destination).Equals(Name(phi.Sources.Values.FirstOrDefault()), StringComparison.OrdinalIgnoreCase))
                    Define(state, phi.Destination, Sym.Unknown);
                return true;

            case IrAssign assign:
                Define(state, assign.Destination, Eval(assign.Value, state));
                return true;

            case IrBinary binary:
            {
                var result = Sym.Unknown;
                var left = Eval(binary.Left, state);
                var right = Eval(binary.Right, state);
                if (binary.Op == "add")
                {
                    if (left.Kind == SymKind.Frame && IsConstant(binary.Right, out var c1) && c1 is >= -0x10000 and <= 0x10000)
                        result = Sym.Frame(left.Value + (int)c1);
                    else if (right.Kind == SymKind.Frame && IsConstant(binary.Left, out var c2) && c2 is >= -0x10000 and <= 0x10000)
                        result = Sym.Frame(right.Value + (int)c2);
                }
                else if (binary.Op == "sub" && left.Kind == SymKind.Frame &&
                         IsConstant(binary.Right, out var c3) && c3 is >= -0x10000 and <= 0x10000)
                {
                    result = Sym.Frame(left.Value - (int)c3);
                }
                Define(state, binary.Destination, result);
                return true;
            }

            case IrLoad load:
            {
                var value = Sym.Unknown;
                if (load.SizeBytes == 4 && TrySlot(load.Address, state, out var slot) &&
                    state.Slots.TryGetValue(slot, out var stored))
                {
                    value = stored;
                }
                Define(state, load.Destination, value);
                return true;
            }

            case IrStore store:
            {
                if (TrySlot(store.Address, state, out var slot))
                {
                    state.KillSlots(slot, Math.Max(store.SizeBytes, 1));
                    var source = Eval(store.Source, state);
                    if (store.SizeBytes == 4 && source.Kind != SymKind.Unknown) state.Slots[slot] = source;
                }
                return true;
            }

            case IrCall call:
                return TransferCall(call, state, calleeClobbers);

            case IrIndirectCall indirect:
                // Unknown target: every register but r1 may have changed. Frame slots survive (see remarks).
                state.KillAcrossCall(uint.MaxValue);
                Define(state, indirect.Destination, Sym.Unknown);
                return true;

            case IrResolveGuestMemoryRange range:
                if (Eval(range.Base, state).Kind == SymKind.Frame && range.NeedsWriteAccess) state.Slots.Clear();
                Define(state, range.Destination, Sym.Unknown);
                return true;
            case IrResolvedLoad resolvedLoad:
                Define(state, resolvedLoad.Destination, Sym.Unknown);
                return true;
            case IrResolvedLoadPair pair:
                Define(state, pair.FirstDestination, Sym.Unknown);
                Define(state, pair.SecondDestination, Sym.Unknown);
                return true;
            case IrResolvedPsqLoad psqLoad:
                Define(state, psqLoad.Destination, Sym.Unknown);
                return true;
            case IrResolvedStore:
            case IrResolvedStorePair:
            case IrResolvedPsqStore:
                // Writes go through a range whose base was checked when it was resolved.
                return true;

            default:
                return false;
        }
    }

    private static bool TransferCall(IrCall call, State state, Func<uint, uint> calleeClobbers)
    {
        if (GuestTargetParser.TryParseAddress(call.Target, out var target))
        {
            var thunks = GuestSaveRestoreThunks.Current;
            if (TryThunkStart(target, thunks.SaveGpr, out var saveStart))
            {
                // _save_gpr_N: stw rK, -4*(32-K)(r11) for K = N..31.
                var r11 = state.Get("r11");
                if (r11.Kind != SymKind.Frame) return true;
                for (var register = saveStart; register <= 31; ++register)
                {
                    var slot = r11.Value - 4 * (32 - register);
                    state.KillSlots(slot, 4);
                    var value = state.Get($"r{register}");
                    if (value.Kind != SymKind.Unknown) state.Slots[slot] = value;
                }
                return true;
            }
            if (TryThunkStart(target, thunks.RestGpr, out var restStart))
            {
                // _rest_gpr_N: lwz rK, -4*(32-K)(r11) for K = N..31.
                var r11 = state.Get("r11");
                for (var register = restStart; register <= 31; ++register)
                {
                    var value = Sym.Unknown;
                    if (r11.Kind == SymKind.Frame &&
                        state.Slots.TryGetValue(r11.Value - 4 * (32 - register), out var stored))
                        value = stored;
                    state.Names[$"r{register}"] = value;
                }
                return true;
            }
            if (TryThunkStart(target, thunks.SaveFpr, out _) || TryThunkStart(target, thunks.RestFpr, out _))
            {
                // FPR save/restore thunks touch FPR slots below the GPR save area and no GPR.
                // _save_fpr_N stores fK at -8*(32-K)(r11), so at most the 144 bytes below r11.
                var r11 = state.Get("r11");
                if (r11.Kind == SymKind.Frame) state.KillSlots(r11.Value - 8 * 18, 8 * 18);
                else if (TryThunkStart(target, thunks.SaveFpr, out _)) state.Slots.Clear();
                return true;
            }

            // A guest call (translated or native).
            state.KillAcrossCall(calleeClobbers(target));
            Define(state, call.Destination, Sym.Unknown);
            return true;
        }

        // A runtime helper: its catalogued hidden writes plus the explicit destination.
        var effect = GuestHelperEffectCatalog.Analyze(call);
        if (effect.BoundaryFlags != GuestCallBoundaryFlags.None)
        {
            state.KillAcrossCall(uint.MaxValue);
            state.Slots.Clear();
        }
        else
        {
            state.KillAcrossCall(effect.GprWriteMask);
            // A helper handed a frame address may store through it (psq_st, dcbz, stswi).
            if (call.Arguments.Any(argument => Eval(argument, state).Kind == SymKind.Frame))
                state.Slots.Clear();
        }
        Define(state, call.Destination, Sym.Unknown);
        return true;
    }

    private static bool TryThunkStart(uint address, GuestSaveRestoreThunkRange? range, out int start)
    {
        start = -1;
        if (range is null || address < range.BaseAddress) return false;
        var index = (address - range.BaseAddress) / 4;
        if ((address - range.BaseAddress) % 4 != 0 || range.FirstRegister + index > range.LastRegister) return false;
        start = range.FirstRegister + (int)index;
        return true;
    }
}
