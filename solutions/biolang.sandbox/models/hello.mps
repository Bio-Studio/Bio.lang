<?xml version="1.0" encoding="UTF-8"?>
<model ref="r:4bd9ccf1-8335-40b4-8c0c-c122060e27eb(biolang.sandbox.hello)">
  <persistence version="9" />
  <languages>
    <use id="982ae924-c56d-43b8-bf9b-3f5c024d8417" name="biolang" version="0" />
  </languages>
  <imports>
    <import index="b1" ref="r:0c993f45-929d-45f9-9d69-3700cd9d09b0(biolang.structure)" />
  </imports>
  <registry>
    <language id="982ae924-c56d-43b8-bf9b-3f5c024d8417" name="biolang">
      <concept id="1216773217294795866" name="biolang.structure.Program" flags="ng" index="P">
        <property id="1884610573006956771" name="kind" index="K" />
        <child id="1213598102282157476" name="declarations" index="D" />
      </concept>
      <concept id="8294974582761536635" name="biolang.structure.StreamFork" flags="ng" index="SF">
        <property id="3448351586244509184" name="sig" index="SG" />
        <child id="1969860954542655096" name="methods" index="M" />
      </concept>
      <concept id="8871072310827228693" name="biolang.structure.MethodDeclaration" flags="ng" index="MD">
        <child id="7837838662573443538" name="type" index="TY" />
        <child id="4556063670432814517" name="body" index="BD" />
      </concept>
      <concept id="5584915986708709954" name="biolang.structure.StatementList" flags="ng" index="SL">
        <child id="7239611833870050589" name="statement" index="ST" />
      </concept>
      <concept id="1863416870945978731" name="biolang.structure.PrintStatement" flags="ng" index="PS">
        <property id="8727844243872626817" name="newline" index="NL" />
        <child id="5190897345115790758" name="args" index="AR" />
      </concept>
      <concept id="3810139151939774156" name="biolang.structure.StringLiteral" flags="ng" index="SLIT">
        <property id="6502323082230230422" name="value" index="VL" />
      </concept>
      <concept id="4489500828230833246" name="biolang.structure.BaseType" flags="ng" index="BT">
        <property id="4800748721218573412" name="kind" index="BKT" />
      </concept>
    </language>
    <language id="ceab5195-25ea-4f22-9b92-103b95ca8c0c" name="jetbrains.mps.lang.core">
      <concept id="1169194658468" name="jetbrains.mps.lang.core.structure.INamedConcept" flags="ngI" index="TrEIO">
        <property id="1169194664001" name="name" index="TrG5h" />
      </concept>
    </language>
  </registry>
  <node concept="P" id="2aBcD">
    <property role="TrG5h" value="hello" />
    <property role="K" value="main" />
    <node concept="SF" id="2aBcE" role="D">
      <property role="TrG5h" value="Main" />
      <node concept="MD" id="2aBcF" role="M">
        <property role="TrG5h" value="exec" />
        <node concept="BT" id="2aBcF1" role="TY">
          <property role="BKT" value="void" />
        </node>
        <node concept="SL" id="2aBcG" role="BD">
          <node concept="PS" id="2aBcH" role="ST">
            <property role="NL" value="true" />
            <node concept="SLIT" id="2aBcI" role="AR">
              <property role="VL" value="Hello, BioLang from MPS!" />
            </node>
          </node>
        </node>
      </node>
    </node>
  </node>
</model>
