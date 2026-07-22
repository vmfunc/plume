{ lib
, stdenv
, cmake
, ninja
, pkg-config
, clang_18
, ftxui
, curl
, sqlite
, nlohmann_json
, tomlplusplus
, luajit
, sol2
, tree-sitter
, chafa
, spdlog
, doctest
, stb
, scdoc
, libsecret ? null
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "plume";
  version = "0.1.0";

  src = lib.cleanSourceWith {
    src = ../.;
    filter = path: type:
      let base = baseNameOf path;
      in !(base == "build" || base == "result" || base == "docs" || base == ".direnv");
  };

  nativeBuildInputs = [ cmake ninja pkg-config scdoc ];

  buildInputs = [
    ftxui
    curl
    sqlite
    nlohmann_json
    tomlplusplus
    luajit
    sol2
    tree-sitter
    chafa
    spdlog
    doctest
    stb
  ] ++ lib.optionals stdenv.isLinux [ libsecret ];

  cmakeFlags = [
    "-DPLUME_VERSION=${finalAttrs.version}"
    "-DCMAKE_BUILD_TYPE=Release"
  ];

  doCheck = false;
  checkPhase = ''
    runHook preCheck
    ctest --output-on-failure
    runHook postCheck
  '';

  meta = with lib; {
    description = "a quill for terminals — talk to models, weave the branches";
    homepage = "https://git.collar.sh/quaver/plume";
    license = licenses.mit;
    mainProgram = "plume";
    platforms = platforms.unix;
  };
})
