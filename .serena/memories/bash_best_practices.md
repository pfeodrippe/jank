# Bash Best Practices for Debugging

## Always Use `tee` for Long-Running Commands

When running build commands or any command that produces a lot of output, ALWAYS use `tee` to capture the output to a file while also displaying it:

```bash
make sdf-ios-simulator-run 2>&1 | tee /tmp/build.log
```

This way:
1. You can see the output in real-time
2. You have a log file to reference later if the output gets truncated
3. You can search through the log for errors

## Example Usage

```bash
# Build and capture output
cd /Users/pfeodrippe/dev/something && make sdf-ios-simulator-run 2>&1 | tee /tmp/ios-build.log

# Then check for errors
grep -i error /tmp/ios-build.log
```
