// Real mapped DSO fixture for the SONAME symlink-retarget regression.
extern "C" int kwin_abi_fixture() {
  return KWIN_ABI_FIXTURE_VALUE;
}
