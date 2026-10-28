package io.github.longcontroller;

import java.util.Objects;

/**
 * Fail-closed preflight model. A VERIFIED input must come from stored device evidence;
 * a target process is never allowed to assert that status for itself.
 */
public final class LongSafetyGate {
    public enum Scenario { GENERAL_MEDIA, UNCLASSIFIED, AUTHENTICITY_REQUIRED }
    public enum Evidence { NOT_RUN, CAPABILITY_ONLY, FRAMES_OBSERVED, VERIFIED }
    public enum Result { ENABLE_VERIFIED, RESEARCH_ONLY, KEEP_NATIVE, ABORT_AND_RECOVER }
    public enum Reason {
        OK,
        PHYSICAL_CAMERA_UNHEALTHY,
        RECOVERY_NOT_PROVEN,
        AUTHENTICITY_REQUIRED,
        USER_NOT_CONSENTED,
        ENVIRONMENT_CHANGED,
        BACKEND_UNSUPPORTED,
        EVIDENCE_INSUFFICIENT,
        UNCLASSIFIED_REQUIRES_REVIEW,
        RESEARCH_REQUIRES_OPT_IN
    }

    public record Input(
            Scenario scenario,
            Evidence evidence,
            boolean userConsented,
            boolean physicalCameraHealthy,
            boolean recoveryVerified,
            boolean environmentMatchesProfile,
            boolean backendCompatible,
            boolean researchMode,
            boolean researchOptIn,
            boolean exactConsumerAndSessionMatch) {
        public Input {
            Objects.requireNonNull(scenario, "scenario");
            Objects.requireNonNull(evidence, "evidence");
        }
    }

    public record Decision(Result result, Reason reason) {}

    private LongSafetyGate() {}

    public static Decision evaluate(Input in) {
        if (!in.physicalCameraHealthy()) {
            return new Decision(Result.ABORT_AND_RECOVER, Reason.PHYSICAL_CAMERA_UNHEALTHY);
        }
        if (!in.recoveryVerified()) {
            return new Decision(Result.KEEP_NATIVE, Reason.RECOVERY_NOT_PROVEN);
        }
        if (in.scenario() == Scenario.AUTHENTICITY_REQUIRED) {
            return new Decision(Result.KEEP_NATIVE, Reason.AUTHENTICITY_REQUIRED);
        }
        if (!in.userConsented()) {
            return new Decision(Result.KEEP_NATIVE, Reason.USER_NOT_CONSENTED);
        }
        if (!in.environmentMatchesProfile()) {
            return new Decision(Result.KEEP_NATIVE, Reason.ENVIRONMENT_CHANGED);
        }
        if (!in.backendCompatible()) {
            return new Decision(Result.KEEP_NATIVE, Reason.BACKEND_UNSUPPORTED);
        }
        if (in.scenario() == Scenario.UNCLASSIFIED) {
            return new Decision(Result.KEEP_NATIVE, Reason.UNCLASSIFIED_REQUIRES_REVIEW);
        }
        if (in.evidence() == Evidence.VERIFIED && in.exactConsumerAndSessionMatch()) {
            return new Decision(Result.ENABLE_VERIFIED, Reason.OK);
        }
        if (in.researchMode() && in.researchOptIn()) {
            return new Decision(Result.RESEARCH_ONLY, Reason.EVIDENCE_INSUFFICIENT);
        }
        if (in.researchMode()) {
            return new Decision(Result.KEEP_NATIVE, Reason.RESEARCH_REQUIRES_OPT_IN);
        }
        return new Decision(Result.KEEP_NATIVE, Reason.EVIDENCE_INSUFFICIENT);
    }
}
