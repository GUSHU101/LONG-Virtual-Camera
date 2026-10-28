package io.github.longcontroller;

import org.junit.Test;
import static org.junit.Assert.*;
import static io.github.longcontroller.LongSafetyGate.*;

public class LongSafetyGateTest {
    @Test public void exhaustiveVerifiedDecisionRequiresEveryPrerequisite() {
        for (Scenario scenario : Scenario.values()) {
            for (Evidence evidence : Evidence.values()) {
                for (int flags = 0; flags < 256; flags++) {
                    Input input = new Input(scenario, evidence, bit(flags,0), bit(flags,1), bit(flags,2),
                            bit(flags,3), bit(flags,4), bit(flags,5), bit(flags,6), bit(flags,7));
                    Decision decision = evaluate(input);
                    boolean eligible = scenario == Scenario.GENERAL_MEDIA && evidence == Evidence.VERIFIED
                            && bit(flags,0) && bit(flags,1) && bit(flags,2) && bit(flags,3) && bit(flags,4) && bit(flags,7);
                    assertEquals(eligible, decision.result() == Result.ENABLE_VERIFIED);
                    if (decision.result() == Result.RESEARCH_ONLY) {
                        assertEquals(Scenario.GENERAL_MEDIA, scenario);
                        assertTrue(bit(flags,0) && bit(flags,1) && bit(flags,2) && bit(flags,3)
                                && bit(flags,4) && bit(flags,5) && bit(flags,6));
                    }
                    if (!bit(flags,1)) assertEquals(Result.ABORT_AND_RECOVER, decision.result());
                }
            }
        }
    }
    @Test public void noEvidenceDoesNotBecomeVerified() {
        assertEquals(Result.KEEP_NATIVE, evaluate(new Input(Scenario.GENERAL_MEDIA, Evidence.NOT_RUN,
                true, true, true, true, true, false, false, true)).result());
    }
    @Test public void requiresExactConsumerEvenWithVerifiedEvidence() {
        assertEquals(Result.KEEP_NATIVE, evaluate(new Input(Scenario.GENERAL_MEDIA, Evidence.VERIFIED,
                true, true, true, true, true, false, false, false)).result());
    }
    private boolean bit(int bits, int index) { return (bits & (1 << index)) != 0; }
}
