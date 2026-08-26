//
//  AdapterSectionHeader.swift
//  Open GPIB
//
//  Collapsible header for one GPIB adapter.
//
//  Deliberately understated: with a single adapter attached — the usual case —
//  this should read as a quiet label above the instrument list rather than as
//  a piece of chrome competing with it.
//

import SwiftUI

struct AdapterSectionHeader: View {
    let adapter: GPIBAdapter
    let isCollapsed: Bool
    let isRefreshing: Bool
    let toggle: () -> Void

    @State private var isHovering = false

    var body: some View {
        Button(action: toggle) {
            HStack(spacing: 4) {
                Image(systemName: "chevron.right")
                    .font(.system(size: 9, weight: .bold))
                    .rotationEffect(.degrees(isCollapsed ? 0 : 90))
                    .foregroundStyle(.secondary)
                    .opacity(isHovering ? 1 : 0.6)

                Text(adapter.displayName)
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)

                if isRefreshing {
                    ProgressView()
                        .controlSize(.mini)
                        .padding(.leading, 2)
                } else if !adapter.instruments.isEmpty {
                    Text("\(adapter.instruments.count)")
                        .font(.system(size: 10, weight: .medium))
                        .foregroundStyle(.tertiary)
                        .padding(.leading, 2)
                }

                Spacer(minLength: 0)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text(adapter.displayName))
        .accessibilityHint(Text(isCollapsed ? "Expand adapter" : "Collapse adapter"))
        .onHover { hovering in
            withAnimation(.linear(duration: 0.12)) { isHovering = hovering }
        }
    }
}
