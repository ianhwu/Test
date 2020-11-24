// FIXME: We shouldn't be consuming a lot of memory here but we do.
// RUN: %scale-test --begin 1 --end 4 --step 1 --select NumLeafScopes %s --expected-exit-code 1 --invert-result -Xfrontend=-solver-expression-time-threshold=1
// REQUIRES: asserts,no_asan

class God {
  public func isEqual(_ other: God) -> Bool {
    return (
	(self.form == other.form)
%for i in range(1, N):
        && (self.form == other.form)
%end
      )
  }
}
