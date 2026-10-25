@{
    schema = 1
    project = @{
        name = 'libdos'
        type = 'gui'
        'default-target' = 'app'
    }
    dependencies = @{ owner = 'dd' }
    build = @{
        'x64-windows' = @{
            debug = 'debug'
            release = 'release'
        }
    }
    targets = @(
        @{
            id = 'app'
            kind = 'gui'
            'cmake-target' = 'dui'
            'test-label' = 'libdos'
            'debug-path' = 'exe/dui-d{exe}'
            'release-path' = 'exe/dui{exe}'
            platforms = @('x64-windows')
        },
        @{
            id = 'cli'
            kind = 'cli'
            'cmake-target' = 'demu'
            'test-label' = 'libdos'
            'debug-path' = 'exe/demu-d{exe}'
            'release-path' = 'exe/demu{exe}'
            platforms = @('x64-windows')
        }
    )
    commands = @{
        bench = @{
            description = "Compile a C source with libdos's own compiler and run it. Defaults to cc/bench/bench.c run natively; --exe emits an executable first."
            script = 'cmake/project-command.ps1'
            effects = 'write'
            'supports-dry-run' = $true
            'timeout-secs' = 1800
            parameters = @{
                source = @{ type = 'string'; default = 'cc/bench/bench.c'; description = 'C source inside the project.' }
                exe = @{ type = 'boolean'; description = 'Emit an executable into exe/ instead of running natively.' }
                config = @{ type = 'string'; default = 'release'; choices = @('debug', 'release') }
            }
        }
    }
}
