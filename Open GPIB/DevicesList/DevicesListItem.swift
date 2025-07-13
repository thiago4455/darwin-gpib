//
//  DevicesListItem.swift
//  Open GPIB
//
//  Created by Thiago Mattos on 13/07/25.
//


import SwiftUI


public struct DevicesListItem: View {
    let devicePath: URL

    public init(devicePath: URL) {
        self.devicePath = devicePath
    }

    public var body: some View {
        HStack(spacing: 8) {
            Image(nsImage: NSWorkspace.shared.icon(forFile: devicePath.path(percentEncoded: false)))
                .resizable()
                .aspectRatio(contentMode: .fit)
                .frame(width: 32, height: 32)
            VStack(alignment: .leading) {
                Text(devicePath.lastPathComponent)
                    .foregroundColor(.primary)
                    .font(.system(size: 13, weight: .semibold))
                    .lineLimit(1)
                Text(formattedPath(for: devicePath))
                    .foregroundColor(.secondary)
                    .font(.system(size: 11))
                    .lineLimit(1)
                    .truncationMode(.head)
            }
        }
        .frame(height: 36)
        .contentShape(Rectangle())
    }

    func formattedPath(for url: URL) -> String {
        let fullPath = url.deletingLastPathComponent().path
        if let realHome = realUserHomeDirectory(),
           fullPath.hasPrefix(realHome) {
            return "~" + fullPath.dropFirst(realHome.count)
        } else {
            return fullPath
        }
    }

    func realUserHomeDirectory() -> String? {
        if let pw = getpwuid(getuid()), let home = pw.pointee.pw_dir { // swiftlint:disable:this identifier_name
            return String(cString: home)
        }
        return nil
    }
}
