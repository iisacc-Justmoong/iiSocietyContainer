import Foundation

enum SocietyTaskVisibility {
    static func canStart(remembered: String?) -> Bool {
        // Missing active cards may have been dismissed while the host was dead.
        remembered != "started" && remembered != "dismissed"
    }
}

// Presentation state is independent of permission to execute in the background.
struct SocietyTaskState: Codable, Hashable {
    var detail: String
    var completed: Int64
    var total: Int64
    var phase: String
    var updatedAt: Date

    init(detail: String, completed: Int64 = 0, total: Int64 = 1,
         phase: String = "running", updatedAt: Date = Date()) {
        self.detail = String(detail.prefix(180))
        self.completed = max(0, completed)
        self.total = max(1, max(total, self.completed))
        self.phase = phase
        self.updatedAt = updatedAt
    }

    var endsActivity: Bool { phase == "completed" || phase == "cancelled" }
    var fraction: Double { min(1, Double(completed) / Double(total)) }
}
