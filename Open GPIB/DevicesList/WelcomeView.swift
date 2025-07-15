//
//  WelcomeView.swift
//  Open GPIB
//
//  Created by Thiago Mattos on 13/07/25.
//


import SwiftUI
import AppKit
import Foundation

public struct WelcomeView: View {

    @Environment(\.colorScheme)
    private var colorScheme

    @Environment(\.controlActiveState)
    private var controlActiveState

    @State private var isHoveringCloseButton = false
    @State private var appIconAverageColor: Color = .accentColor

    @FocusState.Binding var focusedField: FocusTarget?
    private let dismissWindow: () -> Void

    var isMacOS26: Bool {
        if #available(macOS 26, *) {
            return true
        } else {
            return false
        }
    }

    public init(
        dismissWindow: @escaping () -> Void,
        focusedField: FocusState<FocusTarget?>.Binding
    ) {
        self.dismissWindow = dismissWindow
        self._focusedField = focusedField
    }

    private var appVersion: String { Bundle.versionString ?? "" }
    private var appBuild: String { Bundle.buildString ?? "" }
    private var appVersionPostfix: String { Bundle.versionPostfix ?? "" }

    public var body: some View {
        ZStack(alignment: .topLeading) {
            mainContent
            dismissButton
        }
    }

    private var mainContent: some View {
        VStack(spacing: 0) {
            Spacer().frame(height: 32)
            ZStack {
                if colorScheme == .dark {
                    Rectangle()
                        .frame(width: 104, height: 104)
                        .foregroundColor(appIconAverageColor)
                        .clipShape(RoundedRectangle(cornerRadius: 24))
                        .blur(radius: 64)
                        .opacity(0.5)
                }
                (Image(nsImage: NSApp.applicationIconImage))
                    .resizable()
                    .frame(width: 128, height: 128)
            }

            Text(Bundle.displayName)
                .font(.system(size: 36, weight: .bold))
                .multilineTextAlignment(.center)
//                .lineLimit(2)
                .minimumScaleFactor(0.5)
                .fixedSize(horizontal: false, vertical: true)
            Text("for macOS")
                .font(.system(size: 24, weight: .regular))
                .minimumScaleFactor(0.5)
                .fixedSize(horizontal: false, vertical: true)

            Group {
                Text(String(
                    format: "Version %@%@ (%@)",
                    appVersion, appVersionPostfix, appBuild
                ))
            }
            .foregroundColor(.secondary)
            .font(.system(size: 13.5))

            Spacer().frame(height: 40)

            HStack {
                VStack(alignment: .leading, spacing: isMacOS26 ? 6 : 8) {
                    Spacer()
                    WelcomeButton(
                        iconName: "arrow.clockwise",
                        title: "Refresh devices list",
                        action: {
                        }
                    )
                    .focused($focusedField, equals: .action1)
                    WelcomeButton(
                        iconName: "cable.connector.horizontal",
                        title: "Reconnect all adaptors",
                        action: {
                            ExtensionManager.shared.activate()
                        }
                    )
                    .focused($focusedField, equals: .action1)
                    Spacer()
                }
            }
            Spacer()
        }
        .padding(.top, 20)
        .padding(.horizontal, 56)
        .padding(.bottom, 16)
        .frame(width: 460)
        .frame(maxHeight: .infinity)
        .background {
            if colorScheme == .dark {
                Color(.black).opacity(0.275)
                    .background(.ultraThickMaterial)
            } else {
                Color(.white)
                    .background(.regularMaterial)
            }
        }
        .onAppear {
            if let averageNSColor = NSApp.applicationIconImage.dominantColor() {
                appIconAverageColor = Color(averageNSColor)
            }
        }
    }

    private var dismissButton: some View {
        Button(action: dismissWindow) {
            Image(systemName: "xmark.circle.fill")
                .foregroundColor(isHoveringCloseButton ? Color(.secondaryLabelColor) : Color(.tertiaryLabelColor))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text("Close"))
        .focused($focusedField, equals: .dismissButton)
        .modifier(FocusRingModifier(isFocused: focusedField == .dismissButton, shape: .circle))
        .onHover { hover in
            withAnimation(.linear(duration: 0.15)) {
                isHoveringCloseButton = hover
            }
        }
        .padding(10)
        .transition(.opacity.animation(.easeInOut(duration: 0.25)))
    }
}
