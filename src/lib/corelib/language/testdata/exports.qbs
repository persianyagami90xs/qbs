import qbs 1.0
import "exports_product.qbs" as ProductWithInheritedExportItem

Project {
    Application {
        name: "myapp"
        Depends { name: "mylib" }
        Depends { name: "dummy" }
        dummy.defines: ["BUILD_" + product.name.toUpperCase()]
        dummy.includePaths: ["./app"]
    }

    references: [
        "subdir/exports-mylib.qbs"
    ]

    Application {
        name: "A"
        Depends { name: "B" }
    }
    StaticLibrary {
        name: "B"
        Export {
            Depends { name: "C" }
        }
    }
    StaticLibrary {
        name: "C"
        Export {
            Depends { name: "D" }
        }
    }
    StaticLibrary {
        name: "D"
    }

    Application {
        name: "myapp2"
        Depends { name: "productWithInheritedExportItem" }
    }
    ProductWithInheritedExportItem {
        name: "productWithInheritedExportItem"
        Export {
            dummy.cxxFlags: ["-bar"]
        }
    }
    Application {
        name: "myapp3"
        Depends { name: "productWithInheritedExportItem" }
    }
}
