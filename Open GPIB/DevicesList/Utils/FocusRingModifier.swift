//
//  FocusRingModifier.swift
//  Open GPIB
//
//  Created by Thiago Mattos on 13/07/25.
//


import SwiftUI

struct FocusRingModifier<S: InsettableShape>: ViewModifier {
    let isFocused: Bool
    let shape: S

    func body(content: Content) -> some View {
        if #available(macOS 14.0, *) {
            content
                .focusEffectDisabled()
                .padding(1)
                .background(
                    shape
                        .stroke(
                            isFocused ? Color(NSColor.keyboardFocusIndicatorColor) : Color.clear,
                            lineWidth: 3
                        )
                )
        } else {
            content
        }
    }
}
