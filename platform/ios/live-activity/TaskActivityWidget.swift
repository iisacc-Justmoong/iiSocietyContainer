import ActivityKit
import SwiftUI
import WidgetKit

@available(iOSApplicationExtension 16.2, *)
private struct TaskStatus: View {
    let context: ActivityViewContext<SocietyTaskAttributes>
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label(context.attributes.title, systemImage: context.attributes.symbol)
                .font(.headline)
            Text(context.isStale ? "Open the app to check progress" : context.state.detail)
                .font(.subheadline).lineLimit(2)
            ProgressView(value: context.state.fraction)
                .tint(context.state.phase == "paused" || context.isStale ? .orange : .blue)
            HStack(spacing: 4) {
                Text("Last update")
                Text(context.state.updatedAt, style: .time)
            }.font(.caption).foregroundStyle(.secondary)
        }.padding(16)
    }
}

@available(iOSApplicationExtension 16.2, *)
struct SocietyTaskWidget: Widget {
    var body: some WidgetConfiguration {
        ActivityConfiguration(for: SocietyTaskAttributes.self) { context in
            TaskStatus(context: context)
                .activityBackgroundTint(.black.opacity(0.85))
                .activitySystemActionForegroundColor(.white)
                .foregroundStyle(.white)
        } dynamicIsland: { context in
            DynamicIsland {
                DynamicIslandExpandedRegion(.leading) {
                    Label(context.attributes.title, systemImage: context.attributes.symbol)
                        .font(.headline).lineLimit(1)
                }
                DynamicIslandExpandedRegion(.bottom) {
                    VStack(alignment: .leading, spacing: 8) {
                        Text(context.isStale ? "Open the app to check progress" : context.state.detail)
                            .font(.subheadline).lineLimit(2)
                        ProgressView(value: context.state.fraction).tint(.blue)
                    }
                }
            } compactLeading: {
                Image(systemName: context.attributes.symbol)
            } compactTrailing: {
                Image(systemName: context.isStale || context.state.phase != "running"
                    ? "pause.circle" : "ellipsis")
            } minimal: {
                Image(systemName: context.attributes.symbol)
            }
        }
    }
}

@main
@available(iOSApplicationExtension 16.2, *)
struct SocietyTaskWidgetBundle: WidgetBundle {
    var body: some Widget { SocietyTaskWidget() }
}
