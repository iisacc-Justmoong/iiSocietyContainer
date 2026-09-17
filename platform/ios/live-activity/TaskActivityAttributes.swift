import ActivityKit

@available(iOS 16.2, *)
struct SocietyTaskAttributes: ActivityAttributes {
    typealias ContentState = SocietyTaskState
    var taskID: String
    var title: String
    var symbol: String
}
