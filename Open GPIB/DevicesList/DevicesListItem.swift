//
//  DevicesListItem.swift
//  Open GPIB
//
//  Created by Thiago Mattos on 13/07/25.
//


import SwiftUI


/// One row in the devices panel.
///
/// The icon is a placeholder tile for now — the shape and 32pt slot are sized
/// so per-instrument artwork can drop straight in later without disturbing
/// the row metrics.
public struct DevicesListItem: View {
    private let title: String
    private let subtitle: String
    private let symbolName: String

    public init(title: String, subtitle: String, symbolName: String = "waveform") {
        self.title = title
        self.subtitle = subtitle
        self.symbolName = symbolName
    }

    init(instrument: GPIBInstrument) {
        self.init(title: instrument.title, subtitle: instrument.subtitle)
    }

    public var body: some View {
        HStack(spacing: 8) {
            icon
            VStack(alignment: .leading) {
                Text(title)
                    .foregroundColor(.primary)
                    .font(.system(size: 13, weight: .semibold))
                    .lineLimit(1)
                Text(subtitle)
                    .foregroundColor(.secondary)
                    .font(.system(size: 11))
                    .lineLimit(1)
                    .truncationMode(.tail)
            }
        }
        .frame(height: 36)
        .contentShape(Rectangle())
    }

    private var icon: some View {
        RoundedRectangle(cornerRadius: 6, style: .continuous)
            .fill(Color.secondary.opacity(0.12))
            .frame(width: 32, height: 32)
            .overlay {
                Image(systemName: symbolName)
                    .font(.system(size: 15, weight: .regular))
                    .foregroundStyle(.secondary)
            }
    }
}
