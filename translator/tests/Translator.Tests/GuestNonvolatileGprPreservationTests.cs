using Translator.Core.Analysis;
using Translator.Core.Ir;

namespace Translator.Tests;

public sealed class GuestNonvolatileGprPreservationTests
{
    private const uint Callee = 0x80001000u;
    private const uint Caller = 0x80002000u;
    private const uint R1 = 1u << 1;
    private const uint R30 = 1u << 30;
    private const uint R31 = 1u << 31;

    private static GuestAbiContract Contract(uint gprWrites, bool fence = false) => new(
        0, gprWrites, 0, 0, 0, 0, 0, 0, false, false, false, false, false, false,
        fence ? GuestCallBoundaryFlags.RequiresCompleteContext : GuestCallBoundaryFlags.None,
        Array.Empty<uint>());

    /// <summary>stwu r1,-16(r1); stw r31,12(r1); stw r30,8(r1); body; lwz r31/r30; addi r1,r1,16; blr.</summary>
    private static IrFunction Framed(string name, params IrInstruction[] body)
    {
        var instructions = new List<IrInstruction>
        {
            new IrStore(new IrAddress("r1_0", -16), IrValue.Register("r1_0"), 4),
            new IrBinary("r1_1", IrValue.Register("r1_0"), IrValue.Imm(-16), "add"),
            new IrStore(new IrAddress("r1_1", 12), IrValue.Register("r31_0"), 4),
            new IrStore(new IrAddress("r1_1", 8), IrValue.Register("r30_0"), 4),
            new IrAssign("r31_1", IrValue.Register("r3_0")),
            new IrAssign("r30_1", IrValue.Register("r4_0")),
        };
        instructions.AddRange(body);
        instructions.Add(new IrLoad("r31_2", new IrAddress("r1_1", 12), 4));
        instructions.Add(new IrLoad("r30_2", new IrAddress("r1_1", 8), 4));
        instructions.Add(new IrBinary("r1_2", IrValue.Register("r1_1"), IrValue.Imm(16), "add"));
        instructions.Add(new IrReturn(null));
        return new IrFunction(name, "entry", new[] { new IrBasicBlock("entry", instructions) });
    }

    private static uint Analyze(IrFunction function, IReadOnlyDictionary<uint, GuestAbiContract>? contracts = null) =>
        GuestNonvolatileGprPreservation.Analyze(
            new Dictionary<uint, IrFunction> { [Caller] = function },
            new HashSet<uint>(),
            contracts ?? new Dictionary<uint, GuestAbiContract>())[Caller];

    [Fact]
    public void ProvesPrologueSpillAndEpilogueRestoreAcrossUnknownCalls()
    {
        var function = Framed("framed",
            new IrIndirectCall(string.Empty, IrValue.Register("ctr"), Array.Empty<IrValue>()),
            new IrCall(string.Empty, $"0x{Callee:X8}", Array.Empty<IrValue>()));

        var mask = Analyze(function);

        Assert.Equal(R1 | R30 | R31, mask & (R1 | R30 | R31));
    }

    [Fact]
    public void UntouchedRegisterSurvivesOnlyCallsThatPreserveIt()
    {
        var function = Framed("framed", new IrCall(string.Empty, $"0x{Callee:X8}", Array.Empty<IrValue>()));
        const uint r29 = 1u << 29;

        var clobbered = Analyze(function, new Dictionary<uint, GuestAbiContract> { [Callee] = Contract(r29) });
        var untouched = Analyze(function, new Dictionary<uint, GuestAbiContract> { [Callee] = Contract(1u << 3) });
        var fenced = Analyze(function, new Dictionary<uint, GuestAbiContract> { [Callee] = Contract(0, fence: true) });

        Assert.Equal(0u, clobbered & r29);
        Assert.Equal(r29, untouched & r29);
        Assert.Equal(0u, fenced & r29);
        Assert.Equal(R1 | R30 | R31, fenced & (R1 | R30 | R31));
    }

    [Fact]
    public void RegisterLoadedFromAnotherObjectIsNotPreserved()
    {
        // OSLoadContext-style: r31 comes from a structure the caller passed, not from this frame.
        var function = new IrFunction("load_context", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrLoad("r31_1", new IrAddress("r3_0", 124), 4),
                new IrLoad("r1_1", new IrAddress("r3_0", 4), 4),
                new IrReturn(null)
            })
        });

        var mask = Analyze(function);

        Assert.Equal(0u, mask & (R1 | R31));
    }

    [Fact]
    public void OverwrittenSaveSlotBreaksTheProof()
    {
        var function = Framed("overwritten",
            new IrStore(new IrAddress("r1_1", 12), IrValue.Register("r5_0"), 4));

        var mask = Analyze(function);

        Assert.Equal(0u, mask & R31);
        Assert.Equal(R30, mask & R30);
    }

    [Fact]
    public void EveryReturnMustRestoreTheRegister()
    {
        var function = new IrFunction("two_returns", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBranch("raw", "early", "late")
            }),
            new IrBasicBlock("early", new IrInstruction[]
            {
                new IrAssign("r31_1", IrValue.Register("r3_0")),
                new IrReturn(null)
            }),
            new IrBasicBlock("late", new IrInstruction[]
            {
                new IrReturn(null)
            })
        });

        var mask = Analyze(function);

        Assert.Equal(0u, mask & R31);
        Assert.Equal(1u << 30, mask & (1u << 30));
    }

    [Fact]
    public void TailCallThroughCtrProvesNothing()
    {
        var function = new IrFunction("tail", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrIndirectJump(IrValue.Register("ctr"))
            })
        });

        Assert.Equal(0u, Analyze(function));
    }

    [Fact]
    public void FallthroughBlockIsFollowed()
    {
        // "entry" has no terminator and falls through into "clobber", which must not be skipped.
        var function = new IrFunction("fallthrough", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrAssign("r3_1", IrValue.Imm(0))
            }),
            new IrBasicBlock("clobber", new IrInstruction[]
            {
                new IrAssign("r31_1", IrValue.Imm(7)),
                new IrReturn(null)
            })
        });

        var mask = Analyze(function);

        Assert.Equal(0u, mask & R31);
    }
}
