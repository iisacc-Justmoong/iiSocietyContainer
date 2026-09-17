import ActivityKit
import Foundation
import UIKit

@available(iOS 16.2, *)
@MainActor private final class TaskActivities {
    static let shared = TaskActivities()
    private var tail: Task<Void, Never>?
    private var observations: [String: Task<Void, Never>] = [:]
    private var lastPublished: [String: Date] = [:]
    private let defaults = UserDefaults.standard
    private let preference = "SocietyTaskActivityVisibility"

    func enqueue(_ operation: @escaping @MainActor () async -> Void) {
        let previous = tail
        tail = Task { await previous?.value; await operation() }
    }
    private func remembered(_ id: String) -> String? {
        (defaults.dictionary(forKey: preference) as? [String: String])?[id]
    }
    private func remember(_ id: String, _ value: String) {
        var values = defaults.dictionary(forKey: preference) as? [String: String] ?? [:]
        values[id] = value
        defaults.set(values, forKey: preference)
    }
    func allowRestart(_ id: String) { remember(id, "new") }
    func restore() async {
        for value in Activity<SocietyTaskAttributes>.activities {
            guard value.activityState == .active || value.activityState == .stale else { continue }
            observe(value)
            var state = value.content.state
            state.phase = "paused"
            state.detail = "Work interrupted. Open the app to check."
            await update(value.attributes.taskID, state)
            record("restored", value)
        }
    }
    private func activity(_ id: String) -> Activity<SocietyTaskAttributes>? {
        Activity<SocietyTaskAttributes>.activities.first { $0.attributes.taskID == id && ($0.activityState == .active || $0.activityState == .stale) }
    }
    private func observe(_ activity: Activity<SocietyTaskAttributes>) {
        guard observations[activity.id] == nil else { return }
        observations[activity.id] = Task { [weak self] in
            for await state in activity.activityStateUpdates {
                guard let self else { return }
                if state == .dismissed || state == .ended {
                    if remembered(activity.attributes.taskID) != "finished" {
                        remember(activity.attributes.taskID, "dismissed")
                    }
                    record("state", activity)
                    observations[activity.id] = nil
                    return
                }
            }
        }
    }
    private func record(_ event: String, _ activity: Activity<SocietyTaskAttributes>?) {
        let state = activity?.content.state
        let value: [String: Any] = ["event": event, "recordedAt": ISO8601DateFormatter().string(from: Date()),
            "activityID": activity?.id ?? "", "taskID": activity?.attributes.taskID ?? "",
            "activityState": String(describing: activity?.activityState), "phase": state?.phase ?? "",
            "completed": state?.completed ?? 0, "total": state?.total ?? 1,
            "activeCount": Activity<SocietyTaskAttributes>.activities.filter { $0.activityState == .active || $0.activityState == .stale }.count]
        if let data = try? JSONSerialization.data(withJSONObject: value, options: [.sortedKeys]),
           let directory = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first {
            try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            try? data.write(to: directory.appendingPathComponent("live-activity-state.json"), options: .atomic)
        }
        NSLog("Society ActivityKit: %@ %@ %@", event, activity?.id ?? "", state?.phase ?? "")
    }
    func begin(_ id: String, _ title: String, _ symbol: String, _ detail: String) async {
        if let existing = activity(id) {
            observe(existing)
            record("restored", existing)
            return
        }
        // A missing formerly active card can have been dismissed while the app
        // was terminated. Do not resurrect it on a timer or foreground callback.
        if !SocietyTaskVisibility.canStart(remembered: remembered(id)) { return }
        guard ActivityAuthorizationInfo().areActivitiesEnabled,
              UIApplication.shared.applicationState != .background else { return }
        do {
            let value = try Activity.request(attributes: SocietyTaskAttributes(taskID: id, title: title, symbol: symbol),
                content: ActivityContent(state: SocietyTaskState(detail: detail), staleDate: Date().addingTimeInterval(90)),
                pushType: nil)
            remember(id, "started"); observe(value); record("started", value)
        } catch { NSLog("Society ActivityKit request: %@", error.localizedDescription) }
    }
    func update(_ id: String, _ state: SocietyTaskState) async {
        guard let value = activity(id) else { return }
        // Progress can arrive for each CPU graph batch. Coalesce normal updates,
        // but publish phase transitions and pauses immediately.
        if state.phase == value.content.state.phase,
           Date().timeIntervalSince(lastPublished[id] ?? .distantPast) < 1 { return }
        lastPublished[id] = Date()
        await value.update(ActivityContent(state: state, staleDate: state.phase == "running" ? Date().addingTimeInterval(90) : nil))
        record("updated", value)
    }
    func finish(_ id: String, _ phase: String) async {
        guard let value = activity(id) else { return }
        var state = value.content.state
        state.phase = phase; state.updatedAt = Date()
        switch phase {
        case "completed": state.detail = "Completed"; state.completed = state.total
        case "cancelled": state.detail = "Cancelled"
        default: state.detail = "Work interrupted. Open the app to check."
        }
        if state.endsActivity {
            remember(id, "finished")
            await value.end(ActivityContent(state: state, staleDate: nil), dismissalPolicy: phase == "cancelled" ? .immediate : .default)
            record("finished", value)
        } else {
            await update(id, state) // Failure or execution expiration keeps the card.
        }
    }
}

@_cdecl("society_activity_restore")
func societyActivityRestore() {
    if #available(iOS 16.2, *) { Task { @MainActor in
        let service = TaskActivities.shared
        service.enqueue { await service.restore() }
    } }
}

@_cdecl("society_activity_begin")
func societyActivityBegin(_ id: UnsafePointer<CChar>, _ title: UnsafePointer<CChar>, _ symbol: UnsafePointer<CChar>, _ detail: UnsafePointer<CChar>) {
    let args = (String(cString: id), String(cString: title), String(cString: symbol), String(cString: detail))
    if #available(iOS 16.2, *) { Task { @MainActor in
        let service = TaskActivities.shared
        service.enqueue { await service.begin(args.0, args.1, args.2, args.3) }
    } }
}
@_cdecl("society_activity_update")
func societyActivityUpdate(_ id: UnsafePointer<CChar>, _ detail: UnsafePointer<CChar>, _ completed: Int64, _ total: Int64, _ phase: UnsafePointer<CChar>) {
    let key = String(cString: id)
    let state = SocietyTaskState(detail: String(cString: detail), completed: completed, total: total, phase: String(cString: phase))
    if #available(iOS 16.2, *) { Task { @MainActor in
        let service = TaskActivities.shared
        service.enqueue { await service.update(key, state) }
    } }
}
@_cdecl("society_activity_finish")
func societyActivityFinish(_ id: UnsafePointer<CChar>, _ state: UnsafePointer<CChar>) {
    let key = String(cString: id), phase = String(cString: state)
    if #available(iOS 16.2, *) { Task { @MainActor in
        let service = TaskActivities.shared
        service.enqueue { await service.finish(key, phase) }
    } }
}
@_cdecl("society_activity_allow_restart")
func societyActivityAllowRestart(_ id: UnsafePointer<CChar>) {
    let key = String(cString: id)
    if #available(iOS 16.2, *) { Task { @MainActor in
        let service = TaskActivities.shared
        service.enqueue { service.allowRestart(key) }
    } }
}
