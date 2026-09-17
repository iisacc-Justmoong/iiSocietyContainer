import Foundation

@main struct TaskActivityStateTests {
    static func main() throws {
        let running = SocietyTaskState(detail: "Step 3 of 10", completed: 3, total: 10)
        precondition(!running.endsActivity && running.fraction == 0.3)
        var paused = running
        paused.phase = "paused"
        precondition(!paused.endsActivity && paused.completed == running.completed)
        paused.phase = "failed"
        precondition(!paused.endsActivity, "An execution failure must remain visible")
        paused.phase = "completed"
        precondition(paused.endsActivity)
        paused.phase = "cancelled"
        precondition(paused.endsActivity)
        let invalid = SocietyTaskState(detail: String(repeating: "x", count: 500), completed: -1, total: -2)
        precondition(invalid.completed == 0 && invalid.total == 1 && invalid.detail.count == 180)
        let encoded = try JSONEncoder().encode(running)
        let restored = try JSONDecoder().decode(SocietyTaskState.self, from: encoded)
        precondition(restored == running)
        precondition(SocietyTaskVisibility.canStart(remembered: nil))
        precondition(SocietyTaskVisibility.canStart(remembered: "finished"))
        precondition(SocietyTaskVisibility.canStart(remembered: "new"))
        precondition(!SocietyTaskVisibility.canStart(remembered: "dismissed"))
        precondition(!SocietyTaskVisibility.canStart(remembered: "started"))
        print("Activity presentation survives pause/failure; completion, cancellation, bounds and recovery passed")
    }
}
