//
//  View+Cursor.swift
//  Open GPIB
//
//  Created by Thiago Mattos on 13/07/25.
//

import SwiftUI

extension View {
    func cursor(_ cursor: NSCursor) -> some View {
        onHover {
            if $0 {
                cursor.push()
            } else {
                cursor.pop()
            }
        }
    }
}
