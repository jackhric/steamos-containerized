#include <array>
#include <boost/property_tree/json_parser.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <map>
#include <mutex>
#include <random>
#include <server/https_server.hpp>
#include <server/rest_helpers.hpp>
#include <server/servers.hpp>
#include <server/session.hpp>
#include <server/state.hpp>
#include <thread>

using namespace std::chrono_literals;
namespace bt = boost::property_tree;

namespace HTTPServers {

struct PendingPin {
  std::shared_ptr<std::promise<std::string>> pin;
  std::string client_ip;
};
static std::mutex g_pin_mtx;
static std::map<std::string, PendingPin> g_pending_pins;

static constexpr const char *PIN_HTML = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>steam-stream pairing</title>
<style>
:root{
  --bg:#f4f5f6; --card:#fff; --fg:#101214; --on-fg:#fff; --muted:#6b6f76;
  --line:#e2e4e7; --line-2:#c4c8cd; --accent:var(--fg); --ok:var(--fg); --err:var(--fg);
  --shadow:0 1px 2px rgba(16,18,20,.05),0 14px 40px -18px rgba(16,18,20,.22);
}
@media (prefers-color-scheme:dark){
  :root{--bg:#0d0e10;--card:#151719;--fg:#ececee;--on-fg:#101214;--muted:#8e9298;
        --line:#232629;--line-2:#3a3e43;
        --shadow:0 1px 0 rgba(255,255,255,.03) inset,0 24px 60px -24px rgba(0,0,0,.8);}
}
*{box-sizing:border-box}
html,body{height:100%}
body{
  margin:0;background:var(--bg);color:var(--fg);
  font:400 16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Inter,system-ui,Roboto,sans-serif;
  display:grid;place-items:center;padding:24px;-webkit-font-smoothing:antialiased;
}
.card{
  width:min(420px,100%);background:var(--card);border:1px solid var(--line);
  padding:40px 36px 30px;text-align:center;box-shadow:var(--shadow);
  animation:rise .6s cubic-bezier(.2,.8,.2,1) both;
}
@keyframes rise{from{opacity:0;transform:translateY(10px)}}

.logo{width:172px;height:auto;color:var(--fg);display:block;margin:0 auto 30px}
.logo path{transform-box:fill-box;transform-origin:center;opacity:0}
.lg-mark{animation:pop .8s cubic-bezier(.2,.9,.25,1) .15s both}
.lg-word{animation:slide .8s cubic-bezier(.2,.9,.25,1) .34s both}
.lg-tag {animation:slide .8s cubic-bezier(.2,.9,.25,1) .46s both}
@keyframes pop{from{opacity:0;transform:scale(.72) rotate(-8deg)}to{opacity:1;transform:none}}
@keyframes slide{from{opacity:0;transform:translateX(-8px)}to{opacity:1;transform:none}}

h1{margin:0 0 8px;font-size:21px;font-weight:600;letter-spacing:-.02em;animation:fade .6s ease .5s both}
.sub{margin:0 0 30px;color:var(--muted);font-size:14.5px;animation:fade .6s ease .58s both}
@keyframes fade{from{opacity:0;transform:translateY(6px)}}

.otp{display:flex;gap:10px;justify-content:center;margin-bottom:24px;animation:fade .6s ease .66s both}
.otp input{
  width:62px;height:72px;flex:0 0 auto;
  font:500 30px/1 ui-monospace,SFMono-Regular,Menlo,monospace;
  text-align:center;color:var(--fg);caret-color:transparent;
  background:transparent;border:1.5px solid var(--line-2);border-radius:6px;outline:none;
  transition:border-color .16s,box-shadow .16s,background .16s,transform .16s;
}
.otp input.filled{border-color:var(--fg);animation:tick .2s cubic-bezier(.2,.9,.25,1)}
@keyframes tick{from{transform:scale(.94)}}
.otp input:focus{
  border-color:var(--accent);box-shadow:0 0 0 4px color-mix(in srgb,var(--accent) 16%,transparent);
}
.otp input:focus:placeholder-shown{caret-color:var(--accent)}
.otp input::selection{background:transparent}

button{
  width:100%;height:48px;border:0;border-radius:6px;cursor:pointer;
  font:600 15px/1 inherit;color:var(--on-fg);background:var(--accent);
  transition:filter .18s,transform .12s,opacity .18s;animation:fade .6s ease .74s both;
}
button:hover:not(:disabled){opacity:.85}
button:active:not(:disabled){transform:translateY(1px)}
button:disabled{opacity:.4;cursor:default}
button.busy{color:transparent;position:relative}
button.busy::after{content:"";position:absolute;inset:0;margin:auto;width:17px;height:17px;
  border:2px solid color-mix(in srgb,var(--on-fg) 35%,transparent);border-top-color:var(--on-fg);
  border-radius:50%;animation:spin .7s linear infinite}
@keyframes spin{to{transform:rotate(360deg)}}

.msg{min-height:20px;margin:14px 0 0;font-size:13.5px;color:var(--muted)}
.msg.err{color:var(--err)} .msg.ok{color:var(--ok)}

.shake{animation:shake .4s cubic-bezier(.36,.07,.19,.97)}
@keyframes shake{10%,90%{transform:translateX(-2px)}20%,80%{transform:translateX(3px)}
  30%,50%,70%{transform:translateX(-6px)}40%,60%{transform:translateX(6px)}}
/* monotone states: rejected reads as a tint, accepted inverts */
.otp.bad input{border-color:var(--err);background:color-mix(in srgb,var(--fg) 10%,transparent)}
.otp.good input{border-color:var(--ok);background:var(--ok);color:var(--on-fg)}

.done{display:none;flex-direction:column;align-items:center;gap:12px;padding:6px 0 12px}
.done.show{display:flex;animation:fade .45s ease both}
.check{width:58px;height:58px;border-radius:50%;background:color-mix(in srgb,var(--ok) 14%,transparent);
  display:grid;place-items:center;animation:tick .45s cubic-bezier(.2,.9,.25,1) both}
.check svg{width:26px;height:26px;stroke:var(--ok);stroke-width:2.6;fill:none;stroke-linecap:round;
  stroke-linejoin:round;stroke-dasharray:32;stroke-dashoffset:32;animation:draw .4s ease .15s forwards}
@keyframes draw{to{stroke-dashoffset:0}}
.done p{margin:0;font-size:16px;font-weight:600}
.done span{color:var(--muted);font-size:13.5px}

@media (max-width:400px){.otp input{width:56px;height:66px;font-size:27px}.card{padding:32px 22px 26px}}
@media (prefers-reduced-motion:reduce){*{animation-duration:.01ms!important;animation-iteration-count:1!important;transition-duration:.01ms!important}.logo path{opacity:1}}
</style></head>
<body>
<main class="card">
  <svg class="logo" viewBox="0 0 153.62906 37.449444" role="img" aria-label="SteamOS Containerized" xmlns="http://www.w3.org/2000/svg"><g transform="translate(-10.069458,-43.509386)" fill="currentColor"><path class="lg-mark" d="m 16.036923,48.477116 c 3.455987,-3.19088 7.99121,-4.96173 12.696825,-4.96173 l -0.0091,-0.006 c 3.483504,0 6.900068,0.97223 9.862343,2.80696 2.962275,1.83473 5.351728,4.45876 6.90298,7.57952 1.554321,3.11785 2.203503,6.61061 1.880446,10.07877 -0.323056,3.46816 -1.609196,6.7781 -3.712104,9.55755 -2.102908,2.77653 -4.937389,4.91595 -8.189119,6.17167 -3.248818,1.25259 -6.784181,1.57567 -10.209741,0.93257 -3.422678,-0.64614 -6.598207,-2.23091 -9.170486,-4.58364 -2.569237,-2.35289 -4.434417,-5.37316 -5.382154,-8.7257 l 7.177351,2.96836 c 0.259062,1.28921 0.987452,2.43819 2.045018,3.22157 1.057566,0.78325 2.368074,1.14594 3.678502,1.01486 1.307465,-0.12798 2.523516,-0.74057 3.407304,-1.71892 0.880798,-0.9752 1.371468,-2.24308 1.368425,-3.5597 v -0.24992 l 6.366669,-4.54422 h 0.158485 c 1.398905,0 2.764367,-0.41145 3.925359,-1.18864 1.161176,-0.77412 2.069385,-1.87735 2.602732,-3.16654 0.53639,-1.29227 0.676593,-2.7125 0.405342,-4.08384 -0.274294,-1.3684 -0.944801,-2.62715 -1.932252,-3.61447 -0.987452,-0.99052 -2.246154,-1.66402 -3.617648,-1.93532 -1.368425,-0.27427 -2.788708,-0.13406 -4.080933,0.4023 -1.289183,0.53337 -2.392469,1.43856 -3.169709,2.59969 -0.77716,1.16117 -1.191657,2.52655 -1.191657,3.92535 v 0.0883 l -4.464843,6.4733 h -0.295619 c -1.063652,0 -2.102909,0.32004 -2.983707,0.92038 l -10.036174,-4.14471 c 0.377904,-4.69053 2.508276,-9.067 5.967412,-12.25788 z m 3.468423,22.50731 2.301028,0.94784 c 0.353536,0.14933 0.734483,0.2255 1.121542,0.2255 0.384017,0.006 0.764964,-0.0731 1.1185,-0.21943 0.356579,-0.14627 0.679635,-0.36269 0.953929,-0.63389 0.271251,-0.2713 0.487627,-0.59433 0.636958,-0.94784 0.14629,-0.35658 0.225538,-0.73753 0.225538,-1.12157 0,-0.38396 -0.07311,-0.76499 -0.219434,-1.12151 -0.149347,-0.35658 -0.362691,-0.67967 -0.633942,-0.95089 -0.271251,-0.27435 -0.594307,-0.49072 -0.947843,-0.63701 l -2.380245,-0.98139 c 0.722312,-0.27427 1.508628,-0.33219 2.264463,-0.1676 0.755835,0.16458 1.444598,0.54862 1.9871,1.10022 0.542502,0.54861 0.911278,1.2465 1.063652,2.00541 0.149338,0.75578 0.07924,1.5421 -0.207243,2.2583 -0.286491,0.71927 -0.777161,1.3349 -1.411103,1.77985 -0.630872,0.44495 -1.380622,0.69487 -2.151671,0.71927 -0.774118,0.0275 -1.536038,-0.17372 -2.194348,-0.57298 -0.661352,-0.4023 -1.191657,-0.98441 -1.52691,-1.68236 z m 20.108862,-13.56837 c 0,-0.9295 -0.274293,-1.84078 -0.7924,-2.6149 -0.518107,-0.77417 -1.252617,-1.37758 -2.112063,-1.73111 -0.859446,-0.35661 -1.80729,-0.45107 -2.718594,-0.26819 -0.914321,0.17982 -1.752441,0.62783 -2.410751,1.28612 -0.65831,0.65831 -1.106329,1.49947 -1.28614,2.41077 -0.182856,0.91126 -0.08838,1.85913 0.268208,2.7186 0.356579,0.85947 0.956998,1.59398 1.73109,2.11206 0.774118,0.51811 1.685396,0.79243 2.61493,0.79243 1.249548,0 2.444274,-0.49678 3.328194,-1.37758 0.880797,-0.88382 1.377579,-2.07854 1.377579,-3.3282 z m -8.21055,0 c 0,-0.69791 0.207238,-1.38062 0.594307,-1.96273 0.390102,-0.58211 0.941732,-1.03619 1.587844,-1.30439 0.646113,-0.26824 1.356228,-0.33531 2.041975,-0.19814 0.685747,0.13413 1.31662,0.47241 1.810332,0.96617 0.493739,0.49369 0.832035,1.12454 0.966126,1.81031 0.137144,0.68575 0.06702,1.39584 -0.201155,2.04198 -0.265139,0.64611 -0.719243,1.19776 -1.301379,1.58781 -0.58211,0.38709 -1.264788,0.59436 -1.965775,0.59436 -0.463259,0.006 -0.926518,-0.0884 -1.35927,-0.26519 -0.429737,-0.17674 -0.822881,-0.43585 -1.155092,-0.76494 -0.329168,-0.33223 -0.591264,-0.72231 -0.771075,-1.15204 -0.176773,-0.43278 -0.268182,-0.89297 -0.268182,-1.35928 z"/><g transform="translate(0,0.68724049)"><path class="lg-tag" d="m 57.618142,74.23335 c -0.989125,-0.245491 -1.730244,-0.951121 -2.014147,-1.917702 -0.158252,-0.538778 -0.145965,-1.588483 0.02648,-2.262762 0.473535,-1.851501 2.105932,-3.105136 4.043295,-3.105136 0.754005,0 1.676186,0.254174 2.071928,0.571073 0.06495,0.052 -0.01169,0.210016 -0.313356,0.646004 l -0.399334,0.577159 -0.318177,-0.161236 c -1.270236,-0.643677 -2.738407,-0.141758 -3.323463,1.136165 -0.376678,0.822773 -0.373073,2.015936 0.0078,2.591516 0.470648,0.711195 1.521866,0.852017 2.541156,0.340406 0.255794,-0.128388 0.487709,-0.243587 0.515366,-0.256004 0.02766,-0.01237 0.167566,0.214459 0.31091,0.504149 0.305262,0.616923 0.323648,0.580845 -0.502559,0.986182 -0.83877,0.411504 -1.856275,0.546167 -2.645935,0.350186 z m 6.255016,-0.0091 c -0.58533,-0.15764 -1.016432,-0.413653 -1.396139,-0.829112 -1.473732,-1.612495 -0.658736,-4.848689 1.507798,-5.987173 1.447671,-0.760723 3.31231,-0.557241 4.228168,0.461415 0.446355,0.496451 0.606731,0.971145 0.643066,1.903381 0.03625,0.930063 -0.0652,1.404464 -0.482325,2.255342 -0.822419,1.677636 -2.815173,2.650043 -4.500568,2.196147 z m 1.897833,-1.520551 c 0.840286,-0.393791 1.498995,-1.561299 1.497785,-2.654722 -0.0013,-1.087565 -0.531966,-1.635604 -1.587126,-1.638785 -0.802401,-0.0024 -1.366363,0.300894 -1.818576,0.978095 -0.364441,0.545751 -0.517918,1.1423 -0.486279,1.89011 0.0216,0.510406 0.05341,0.636557 0.23404,0.928119 0.292999,0.472936 0.669821,0.662033 1.317505,0.661138 0.371746,-5.21e-4 0.580589,-0.04117 0.842651,-0.163955 z m 3.678685,1.33182 c 0.0195,-0.08623 0.297278,-1.670745 0.617275,-3.521149 l 0.581813,-3.364363 0.656162,0.01961 0.656162,0.01961 1.170419,2.090485 c 0.64373,1.149753 1.182967,2.121826 1.198304,2.16015 0.01534,0.03832 0.04477,0.05403 0.06541,0.03483 0.02063,-0.0192 0.204476,-0.99907 0.408528,-2.177579 l 0.371002,-2.14273 h 0.779091 c 0.70615,0 0.776483,0.01139 0.75123,0.121944 -0.01532,0.06705 -0.263931,1.485978 -0.552462,3.153127 -0.288531,1.667157 -0.557495,3.195812 -0.597698,3.397023 l -0.0731,0.365834 -0.639622,-0.0024 -0.639623,-0.0024 -1.219444,-2.15596 C 72.312429,70.845768 71.74577,69.874639 71.72388,69.873484 71.702,69.872673 71.515574,70.843457 71.309624,72.031551 l -0.374454,2.160151 h -0.76048 c -0.752699,0 -0.760118,-0.0016 -0.725018,-0.156786 z m 12.45444,0.104524 c 8.84e-4,-0.02872 0.857669,-1.60444 1.903933,-3.501548 l 1.902301,-3.449277 0.76219,-0.01953 0.762191,-0.01953 0.723282,3.433988 c 0.397806,1.888695 0.737903,3.473183 0.755778,3.521092 0.02484,0.06664 -0.163941,0.0871 -0.804008,0.0871 h -0.83649 l -0.07854,-0.470357 c -0.04321,-0.258698 -0.09595,-0.556589 -0.117213,-0.661984 l -0.03867,-0.191634 h -1.353729 -1.353729 l -0.328839,0.661984 -0.328841,0.661985 h -0.785627 c -0.432093,0 -0.784887,-0.02351 -0.783989,-0.05224 z m 4.714851,-2.613095 c -0.02036,-0.08622 -0.124538,-0.697688 -0.231489,-1.35881 -0.106951,-0.661105 -0.211647,-1.202015 -0.232661,-1.202015 -0.02102,0 -0.343459,0.611463 -0.716549,1.358809 l -0.678343,1.358802 h 0.948037 c 0.947668,0 0.948024,-5.8e-5 0.911005,-0.156786 z m 2.912778,2.508571 c 0.02012,-0.08623 0.286541,-1.599216 0.592113,-3.362182 0.305581,-1.762966 0.572559,-3.275941 0.593279,-3.362174 0.03612,-0.150228 0.06825,-0.156786 0.768015,-0.156786 0.537767,0 0.730335,0.02295 0.730335,0.08709 0,0.04793 -0.266536,1.613374 -0.592298,3.47883 -0.325762,1.865464 -0.592298,3.409806 -0.592298,3.431873 0,0.02205 -0.345531,0.04011 -0.767838,0.04011 -0.760637,0 -0.767492,-0.0016 -0.731308,-0.156786 z m 2.857012,0.06965 c 0.01629,-0.04793 0.282229,-1.545205 0.591089,-3.327333 0.308861,-1.782137 0.579296,-3.32647 0.600967,-3.431865 l 0.03936,-0.191627 h 0.64346 0.643445 l 1.215016,2.190809 c 0.98403,1.774326 1.224355,2.161803 1.264166,2.038215 0.02698,-0.08393 0.191735,-0.983562 0.366015,-1.999183 0.17428,-1.01562 0.334953,-1.932819 0.357059,-2.038214 l 0.04017,-0.191627 h 0.769681 0.769682 l -0.037,0.191627 c -0.02034,0.105395 -0.297243,1.688931 -0.615266,3.518968 l -0.578204,3.327333 H 97.819772 97.181148 L 95.945434,72.02767 c -0.679645,-1.190526 -1.251849,-2.148466 -1.271559,-2.128759 -0.01968,0.01969 -0.188396,0.907549 -0.374853,1.972974 -0.18645,1.065425 -0.357737,2.023365 -0.380617,2.128759 l -0.04157,0.191628 h -0.758786 c -0.576318,0 -0.751682,-0.02091 -0.729252,-0.0871 z m 7.462652,-0.174206 c 0.0232,-0.143719 0.297961,-1.719411 0.610611,-3.50154 l 0.56845,-3.240238 2.31686,-0.01839 c 1.89041,-0.01497 2.31687,-0.0016 2.31687,0.07429 0,0.09403 -0.11466,0.789795 -0.17739,1.076393 l -0.0344,0.156786 h -1.56338 -1.56337 l -0.0459,0.261309 c -0.0252,0.143719 -0.0846,0.472969 -0.13176,0.731667 l -0.0859,0.470357 h 1.38844 1.38843 l -0.0386,0.191627 c -0.0212,0.105395 -0.074,0.403287 -0.11722,0.661984 l -0.0785,0.470357 h -1.38649 c -0.76258,0 -1.3865,0.0096 -1.3865,0.0214 0,0.0118 -0.0627,0.36275 -0.13936,0.77995 -0.0767,0.4172 -0.13937,0.768176 -0.13937,0.77995 0,0.0118 0.72478,0.0214 1.61061,0.0214 h 1.61061 l -0.0484,0.330993 c -0.0267,0.182041 -0.0798,0.479942 -0.11821,0.661984 l -0.0697,0.330991 h -2.36424 -2.364251 z m 12.405791,0.139365 c 0.0164,-0.06705 0.29462,-1.639102 0.61828,-3.493403 0.32367,-1.854301 0.60109,-3.384062 0.6165,-3.399473 0.0154,-0.01538 0.35529,-0.01904 0.7553,-0.0081 l 0.7273,0.01986 -0.56725,3.240238 c -0.31199,1.782129 -0.58953,3.357829 -0.61677,3.501547 l -0.0495,0.261309 h -0.75683 c -0.68569,0 -0.75402,-0.01147 -0.72702,-0.121943 z m 2.64512,-0.368901 0.0935,-0.490845 1.9366,-2.165245 c 1.06512,-1.190876 2.00941,-2.255642 2.09843,-2.36613 l 0.16183,-0.200895 h -1.61244 c -1.50264,0 -1.61005,-0.0083 -1.57718,-0.121944 0.0195,-0.06705 0.0747,-0.364963 0.12281,-0.661984 l 0.0875,-0.54004 h 2.60605 2.60604 l -0.05,0.365833 c -0.0275,0.201204 -0.0746,0.428543 -0.10463,0.505199 -0.03,0.07665 -1.01215,1.189826 -2.18249,2.473721 l -2.1279,2.334366 1.84803,0.01855 1.84801,0.01855 -0.0384,0.190481 c -0.0212,0.104768 -0.0737,0.402139 -0.11696,0.660836 l -0.0785,0.470357 h -2.80689 -2.80689 z m 6.62033,0.229536 c 0.0232,-0.143719 0.29797,-1.719411 0.61062,-3.50154 l 0.56844,-3.240238 2.31687,-0.01839 c 1.89041,-0.01497 2.31687,-0.0016 2.31687,0.07429 0,0.09403 -0.11467,0.789795 -0.17739,1.076393 l -0.0344,0.156786 h -1.56338 -1.56336 l -0.0459,0.261309 c -0.0252,0.143719 -0.0846,0.472969 -0.13177,0.731667 l -0.0859,0.470357 h 1.38844 1.38843 l -0.0386,0.191627 c -0.0213,0.105395 -0.074,0.403287 -0.11722,0.661984 l -0.0785,0.470357 h -1.38651 c -0.76257,0 -1.38649,0.0096 -1.38649,0.0214 0,0.0118 -0.0627,0.36275 -0.13937,0.77995 -0.0767,0.4172 -0.13936,0.768176 -0.13936,0.77995 0,0.0118 0.72477,0.0214 1.61061,0.0214 h 1.61061 l -0.0484,0.330993 c -0.0267,0.182041 -0.0798,0.479942 -0.1182,0.661984 l -0.0697,0.330991 h -2.36426 -2.36425 z m 5.77496,0.111813 c 0.021,-0.09024 0.2887,-1.606506 0.59491,-3.369472 0.30621,-1.762967 0.57402,-3.278904 0.59512,-3.368757 l 0.0383,-0.16336 1.5823,0.02978 c 1.39024,0.02611 1.63078,0.0476 1.98178,0.176631 0.76,0.279405 1.26062,0.718387 1.55616,1.364545 0.42856,0.937029 0.36116,2.211787 -0.17408,3.291931 -0.47265,0.953863 -1.39993,1.700159 -2.51639,2.025277 -0.28122,0.08189 -0.81146,0.119373 -2.03821,0.144109 l -1.65811,0.03344 z m 3.65076,-1.487279 c 0.78985,-0.433571 1.25367,-1.209574 1.31737,-2.204032 0.0532,-0.829103 -0.16505,-1.328744 -0.70296,-1.609988 -0.22968,-0.12009 -0.44301,-0.157583 -1.02194,-0.179593 l -0.73167,-0.02783 -0.367,2.08002 c -0.20185,1.14401 -0.36648,2.10701 -0.36584,2.139996 5.3e-4,0.03296 0.33827,0.04785 0.75026,0.03296 0.6699,-0.02417 0.78849,-0.04857 1.12178,-0.231562 z m -52.291789,1.375465 c 0.06008,-0.286053 0.946381,-5.408436 0.946381,-5.469607 0,-0.02897 -0.472848,-0.05272 -1.050774,-0.05272 h -1.050774 l 0.03867,-0.191628 c 0.02127,-0.105394 0.07401,-0.403286 0.117214,-0.661984 l 0.07854,-0.470357 h 2.849828 2.849829 l -0.0043,0.191628 c -0.0024,0.105394 -0.04311,0.395442 -0.09057,0.644563 l -0.08628,0.452936 -1.054044,0.0192 -1.054046,0.0192 -0.03887,0.189837 c -0.06018,0.29397 -0.946978,5.419013 -0.946978,5.472862 0,0.0262 -0.34742,0.04768 -0.772044,0.04768 h -0.772046 z m 26.964779,0.06965 c 0.0199,-0.06705 0.28882,-1.580047 0.59752,-3.362183 0.3087,-1.782129 0.57812,-3.314021 0.59872,-3.404217 l 0.0375,-0.163986 1.65183,0.03173 c 1.83652,0.03531 2.14792,0.09233 2.67656,0.490113 0.18098,0.136191 0.37676,0.376411 0.49541,0.60794 0.17791,0.347078 0.19405,0.438006 0.17313,0.975075 -0.018,0.461603 -0.0639,0.679894 -0.20807,0.989242 -0.21642,0.46445 -0.68465,0.958737 -1.11274,1.174677 -0.16653,0.084 -0.31089,0.159592 -0.32079,0.167973 -0.01,0.0084 0.1737,0.435695 0.40801,0.949576 0.23431,0.513871 0.49939,1.098948 0.58905,1.300151 l 0.16303,0.365833 -0.84977,-8.1e-4 -0.84978,-8.11e-4 -0.48042,-1.151349 -0.48041,-1.151357 -0.59573,0.02002 -0.59573,0.02002 -0.0815,0.452936 c -0.0448,0.249113 -0.13576,0.758664 -0.20201,1.132333 l -0.12045,0.679405 h -0.76476 c -0.69569,0 -0.76149,-0.01098 -0.72853,-0.121945 z m 3.75778,-3.585983 c 0.6095,-0.182611 0.99704,-0.859933 0.80531,-1.407426 -0.16268,-0.464474 -0.36787,-0.559258 -1.2729,-0.587989 l -0.78494,-0.0249 -0.15536,0.895596 c -0.0854,0.492586 -0.17252,0.966157 -0.19351,1.05239 -0.0376,0.154475 -0.0282,0.156785 0.64069,0.156785 0.37339,0 0.8057,-0.038 0.96071,-0.08443 z"/><path class="lg-word" d="m 138.02141,63.554542 c 3.31893,0 6.75058,-2.30103 6.75058,-7.44855 0,-5.15673 -3.43165,-7.3451 -6.75058,-7.3451 -3.31894,0 -6.76884,2.18218 -6.76884,7.3451 0,5.16281 3.4499,7.44855 6.76884,7.44855 z m -0.067,-1.35319 c -2.77945,0 -5.14747,-2.11815 -5.14747,-6.09547 v -0.0124 c 0,-4.03516 2.4534,-5.95206 5.22367,-5.95206 2.77045,0 5.19324,1.89568 5.19324,5.96423 0,4.07168 -2.48997,6.09547 -5.26944,6.09547 z m 18.95369,-12.13591 -0.68268,1.16729 c -1.11545,-0.74369 -2.41988,-1.15207 -3.76078,-1.17337 -1.87741,0 -3.08743,0.85942 -3.08743,2.3894 0,1.52691 1.61833,2.17609 3.72137,3.02022 0.16152,0.067 0.3261,0.13103 0.49374,0.20113 2.24006,0.9052 3.69067,1.82862 3.69067,3.79757 0,2.40768 -1.82253,4.07485 -4.95856,4.07485 -1.661,-0.0124 -3.2885,-0.46321 -4.71778,-1.30742 l 0.57906,-1.24044 c 1.23738,0.71313 2.63324,1.10024 4.06268,1.12456 2.48997,0 3.49277,-1.09718 3.49277,-2.49913 0,-1.47507 -1.41719,-2.14561 -3.57505,-2.97762 -2.3132,-0.88998 -4.31245,-1.77683 -4.31245,-4.22724 0,-2.10593 1.74329,-3.65417 4.58364,-3.65417 1.58785,-0.0275 3.14537,0.4267 4.47093,1.30445 z m -92.482456,2.32847 1.28614,-2.23404 c -1.414145,-0.95697 -3.093508,-1.44459 -4.800071,-1.39887 -3.047735,0 -5.193242,1.52691 -5.193242,4.18465 0,2.35585 1.609196,3.37079 3.925359,4.14496 0.152381,0.0486 0.298661,0.0975 0.438864,0.14325 h 0.0029 c 1.770724,0.58211 2.852738,0.9356 2.852738,1.93834 0,0.94784 -0.838121,1.59086 -2.575322,1.59086 -1.37451,-0.0245 -2.727589,-0.37483 -3.943614,-1.02097 l -0.926598,2.48084 c 1.532996,0.86862 3.267075,1.3258 5.028671,1.31966 3.282421,0 5.577417,-1.63658 5.577417,-4.55031 0,-2.12727 -1.337945,-3.09641 -3.663421,-3.91927 -0.338296,-0.12194 -0.65831,-0.22858 -0.960041,-0.33218 -1.621393,-0.55163 -2.657475,-0.90519 -2.657475,-1.9109 0,-0.9814 0.816769,-1.52998 2.142543,-1.52998 1.237377,0.0216 2.441205,0.39925 3.465248,1.09413 z m 11.441097,-0.81986 v 11.70622 h -3.00197 v -11.70622 h -4.33387 v -2.57223 h 11.66654 v 2.57223 z m 10.42008,3.2123 v -3.2213 l 6.64713,0.0124 v -2.57223 h -9.63692 v 14.27533 h 9.63692 v -2.57223 h -6.64713 v -3.34936 h 5.72982 v -2.57222 z m 13.32759,5.7359 -0.94784,2.75829 h -3.13293 l 5.354899,-14.29068 h 3.00196 l 5.51947,14.29068 h -3.24273 l -0.96308,-2.77337 z m 2.761199,-8.10683 -1.95054,5.72056 h 3.95288 z m 18.27106,10.46877 3.91028,-8.35687 v 8.75295 h 2.86491 v -14.27559 h -2.88608 l -4.63259,10.31663 -4.80324,-10.31663 h -2.86782 v 14.27559 h 2.86782 v -8.67383 l 3.86132,8.27748 z m 39.04721,-13.45565 v 2.32542 h -0.3505 v -2.32542 h -0.86251 v -0.30779 h 2.07245 v 0.30779 z m 3.66633,2.32542 h 0.3383 l -0.009,-2.63321 h -0.35354 l -0.95398,2.1517 -0.99356,-2.1517 h -0.3505 v 2.63321 h 0.34134 v -1.94138 l 0.89297,1.90789 h 0.19201 l 0.89604,-1.90789 v 1.94138"/></g></g></svg>
  <div id="form">
    <h1>Pair this device</h1>
    <p class="sub">Enter the 4-digit code shown in Moonlight</p>
    <div class="otp" id="otp">
      <input inputmode="numeric" autocomplete="one-time-code" maxlength="1" placeholder=" " aria-label="Digit 1" autofocus>
      <input inputmode="numeric" maxlength="1" placeholder=" " aria-label="Digit 2">
      <input inputmode="numeric" maxlength="1" placeholder=" " aria-label="Digit 3">
      <input inputmode="numeric" maxlength="1" placeholder=" " aria-label="Digit 4">
    </div>
    <button id="go" disabled>Pair</button>
    <p class="msg" id="msg"></p>
  </div>
  <div class="done" id="done">
    <div class="check"><svg viewBox="0 0 24 24"><path d="M4 12.5l5.2 5.2L20 7"/></svg></div>
    <p>Paired</p><span>You can return to Moonlight.</span>
  </div>
</main>
<script>
(() => {
  const secret = location.hash.slice(1);
  const otp = document.getElementById('otp');
  const boxes = [...otp.querySelectorAll('input')];
  const go = document.getElementById('go');
  const msg = document.getElementById('msg');
  const form = document.getElementById('form');
  const done = document.getElementById('done');
  const send = async pin => {
    const r = await fetch('/pin/', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ pin, secret })
    });
    if (!r.ok) throw new Error(await r.text());
  };

  const code = () => boxes.map(b => b.value).join('');
  const sync = () => {
    boxes.forEach(b => b.classList.toggle('filled', !!b.value));
    go.disabled = code().length !== boxes.length;
  };
  const focusFirstEmpty = () => (boxes.find(b => !b.value) || boxes.at(-1)).focus();

  boxes.forEach((box, i) => {
    box.addEventListener('input', () => {
      box.value = box.value.replace(/\D/g, '').slice(-1);
      sync();
      if (box.value && i < boxes.length - 1) boxes[i + 1].focus();
      if (code().length === boxes.length) submit();
    });
    box.addEventListener('keydown', e => {
      if (e.key === 'Backspace' && !box.value && i > 0) { boxes[i - 1].focus(); boxes[i - 1].value = ''; sync(); }
      if (e.key === 'ArrowLeft' && i > 0) boxes[i - 1].focus();
      if (e.key === 'ArrowRight' && i < boxes.length - 1) boxes[i + 1].focus();
      if (e.key === 'Enter') submit();
    });
    box.addEventListener('focus', () => box.select());
    box.addEventListener('paste', e => {
      e.preventDefault();
      const digits = (e.clipboardData.getData('text') || '').replace(/\D/g, '').slice(0, boxes.length);
      boxes.forEach((b, j) => b.value = digits[j] || '');
      sync(); focusFirstEmpty();
      if (code().length === boxes.length) submit();
    });
  });

  const fail = text => {
    msg.textContent = text; msg.className = 'msg err';
    otp.classList.add('bad', 'shake');
    setTimeout(() => otp.classList.remove('shake'), 450);
    setTimeout(() => {
      otp.classList.remove('bad');
      boxes.forEach(b => b.value = ''); sync(); boxes[0].focus();
    }, 900);
  };

  let busy = false;
  async function submit() {
    if (busy || code().length !== boxes.length) return;
    busy = true; go.disabled = true; go.classList.add('busy');
    msg.textContent = ''; msg.className = 'msg';
    try {
      await send(code());
      otp.classList.add('good');
      setTimeout(() => { form.style.display = 'none'; done.classList.add('show'); }, 420);
    } catch (e) {
      fail(e instanceof TypeError ? 'Could not reach the server' : (e.message || 'Pairing failed'));
    } finally {
      busy = false; go.classList.remove('busy'); sync();
    }
  }
  go.addEventListener('click', submit);

  if (!secret) { msg.textContent = 'Missing pairing link -- reopen the URL from the server log.'; msg.className = 'msg err'; }
  sync();
})();
</script>
</body></html>)HTML";

template <class T>
static void serverinfo(const std::shared_ptr<typename SimpleWeb::Server<T>::Response> &response,
                       const std::shared_ptr<typename SimpleWeb::Server<T>::Request> &request,
                       state::AppState &state,
                       bool is_paired) {
  log_req<T>(request);
  bool is_https = std::is_same_v<SimpleWeb::HTTPS, T>;
  auto local_ip = request->local_endpoint().address().to_string();

  auto running = state.sessions->get_running();
  auto xml = moonlight::serverinfo(running != nullptr, // is_busy
                                   running ? 1 : 0,    // currentgame (app id placeholder)
                                   state.https_port,
                                   state.http_port,
                                   state.uuid,
                                   state.hostname,
                                   state.mac_address,
                                   local_ip,
                                   state.display_modes,
                                   is_paired ? 1 : 0,
                                   state.support_hevc,
                                   state.support_av1,
                                   // HTTPS only. Moonlight fetches serverinfo over plain HTTP for
                                   // an unknown host, and anyone who can spoof that would get to
                                   // point a client's USB bridge at their own listener.
                                   is_https ? state.usb_bridge_port : 0);
  send_xml<T>(response, SimpleWeb::StatusCode::success_ok, xml);
}

static std::string env_or(const char *k, const std::string &def) {
  const char *v = std::getenv(k);
  return v ? std::string(v) : def;
}

static std::shared_ptr<session::StreamSession>
create_launch_session(state::AppState &state, const SimpleWeb::CaseInsensitiveMultimap &headers,
                      const std::string &client_ip) {
  auto sess = std::make_shared<session::StreamSession>();
  sess->session_id = state.sessions->next_id();
  sess->client_ip = client_ip;

  sess->app_id = get_header(headers, "appid").value_or("1");
  for (const auto &a : state.apps)
    if (a.id == sess->app_id)
      sess->app_name = a.title;

  // mode = "WIDTHxHEIGHTxFPS"
  auto mode = get_header(headers, "mode").value_or("1920x1080x60");
  int w = 1920, h = 1080, fps = 60;
  std::sscanf(mode.c_str(), "%dx%dx%d", &w, &h, &fps);
  sess->client_width = w;
  sess->client_height = h;
  sess->client_fps = fps;

  // surroundAudioInfo: low 16 bits = channel count.
  int surround = std::stoi(get_header(headers, "surroundAudioInfo").value_or("196610"));
  sess->audio_channel_count = surround & 0xffff;

  // rikey is the AES-128 key (hex), rikeyid the IV seed (decimal).
  sess->aes_key = get_header(headers, "rikey").value_or("");
  sess->aes_iv = get_header(headers, "rikeyid").value_or("");
  if (sess->aes_key.empty() || sess->aes_iv.empty())
    logs::log(logs::warning, "[HTTP] launch missing rikey/rikeyid -- media encryption will fail");

  sess->hevc_supported = state.support_hevc;
  sess->av1_supported = state.support_av1;
  sess->video_stream_port = state.video_stream_port;
  sess->audio_stream_port = state.audio_stream_port;
  sess->control_stream_port = state.control_stream_port;

  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> printable(33, 126);
  for (auto &c : sess->rtp_secret_payload)
    c = static_cast<char>(printable(gen));
  std::uniform_int_distribution<std::uint32_t> u32(0, UINT32_MAX);
  sess->enet_secret_payload = u32(gen);
  std::uniform_int_distribution<> octet(0, 255);
  sess->rtsp_fake_ip =
      std::to_string(octet(gen)) + "." + std::to_string(octet(gen)) + "." + std::to_string(octet(gen)) + "." +
      std::to_string(octet(gen));
  return sess;
}

static void http_pair(const std::shared_ptr<typename SimpleWeb::Server<SimpleWeb::HTTP>::Response> &response,
                      const std::shared_ptr<typename SimpleWeb::Server<SimpleWeb::HTTP>::Request> &request,
                      state::AppState &state) {
  log_req<SimpleWeb::HTTP>(request);
  auto headers = request->parse_query_string();
  auto salt = get_header(headers, "salt");
  auto client_cert_str = get_header(headers, "clientcert");
  auto client_id = get_header(headers, "uniqueid");
  auto client_ip = request->remote_endpoint().address().to_string();

  if (!client_id) {
    send_xml<SimpleWeb::HTTP>(response,
                              SimpleWeb::StatusCode::client_error_bad_request,
                              fail_pair("Received pair request without uniqueid, stopping."));
    return;
  }
  auto cache_key = client_id.value() + "@" + client_ip;

  // Client sends salt + cert; we need the user PIN to derive the AES key.
  if (salt && client_cert_str) {
    if (state.get_pair_cache(cache_key)) {
      send_xml<SimpleWeb::HTTP>(response,
                                SimpleWeb::StatusCode::client_error_bad_request,
                                fail_pair("Out of order pair request (phase 1)"));
      state.remove_pair_cache(cache_key);
      return;
    }
    auto pin_promise = std::make_shared<std::promise<std::string>>();
    auto secret = crypto::str_to_hex(crypto::random(8));
    {
      std::lock_guard<std::mutex> lk(g_pin_mtx);
      for (auto it = g_pending_pins.begin(); it != g_pending_pins.end();) {
        if (it->second.client_ip == client_ip)
          it = g_pending_pins.erase(it);
        else
          ++it;
      }
      g_pending_pins[secret] = {pin_promise, client_ip};
    }
    logs::log(logs::info, ">>> PAIRING: open http://{}:{}/pin/#{} and enter the Moonlight PIN",
              request->local_endpoint().address().to_string(), state.http_port, secret);

    std::thread([response, pin_promise, secret, salt = salt.value(),
                 client_cert_str = client_cert_str.value(), cache_key, &state]() {
      auto fut = pin_promise->get_future();
      if (fut.wait_for(180s) != std::future_status::ready) {
        std::lock_guard<std::mutex> lk(g_pin_mtx);
        g_pending_pins.erase(secret);
        send_xml<SimpleWeb::HTTP>(response, SimpleWeb::StatusCode::client_error_bad_request,
                                  fail_pair("Timed out waiting for PIN"));
        return;
      }
      auto pin = fut.get();
      auto server_pem = x509::get_cert_pem(state.server_cert);
      auto [xml, aes_key] = moonlight::pair::get_server_cert(pin, salt, server_pem);

      state::PairCache pc;
      pc.client_cert = crypto::hex_to_str(client_cert_str, true);
      pc.aes_key = aes_key;
      pc.last_phase = state::PAIR_PHASE::GETSERVERCERT;
      state.set_pair_cache(cache_key, pc);

      send_xml<SimpleWeb::HTTP>(response, SimpleWeb::StatusCode::success_ok, xml);
    }).detach();
    return;
  }

  auto cache_opt = state.get_pair_cache(cache_key);
  if (!cache_opt) {
    send_xml<SimpleWeb::HTTP>(response, SimpleWeb::StatusCode::client_error_bad_request,
                              fail_pair("Unable to find " + cache_key + " in the pairing cache"));
    return;
  }
  auto cache = cache_opt.value();

  auto client_challenge = get_header(headers, "clientchallenge");
  if (client_challenge) {
    if (cache.last_phase != state::PAIR_PHASE::GETSERVERCERT) {
      send_xml<SimpleWeb::HTTP>(response, SimpleWeb::StatusCode::client_error_bad_request,
                                fail_pair("Out of order pair request (phase 2)"));
      state.remove_pair_cache(cache_key);
      return;
    }
    cache.last_phase = state::PAIR_PHASE::CLIENTCHALLENGE;
    auto server_cert_signature = x509::get_cert_signature(state.server_cert);
    auto [xml, server_secret_pair] =
        moonlight::pair::send_server_challenge(cache.aes_key, client_challenge.value(), server_cert_signature);
    cache.server_secret = server_secret_pair.first;
    cache.server_challenge = server_secret_pair.second;
    state.set_pair_cache(cache_key, cache);
    send_xml<SimpleWeb::HTTP>(response, SimpleWeb::StatusCode::success_ok, xml);
    return;
  }

  auto server_challenge = get_header(headers, "serverchallengeresp");
  if (server_challenge && cache.server_secret) {
    if (cache.last_phase != state::PAIR_PHASE::CLIENTCHALLENGE) {
      send_xml<SimpleWeb::HTTP>(response, SimpleWeb::StatusCode::client_error_bad_request,
                                fail_pair("Out of order pair request (phase 3)"));
      state.remove_pair_cache(cache_key);
      return;
    }
    cache.last_phase = state::PAIR_PHASE::SERVERCHALLENGERESP;
    auto [xml, client_hash] = moonlight::pair::get_client_hash(
        cache.aes_key, cache.server_secret.value(), server_challenge.value(),
        x509::get_pkey_content(state.server_pkey));
    cache.client_hash = client_hash;
    state.set_pair_cache(cache_key, cache);
    send_xml<SimpleWeb::HTTP>(response, SimpleWeb::StatusCode::success_ok, xml);
    return;
  }

  auto client_secret = get_header(headers, "clientpairingsecret");
  if (client_secret && cache.server_challenge && cache.client_hash) {
    if (cache.last_phase != state::PAIR_PHASE::SERVERCHALLENGERESP) {
      send_xml<SimpleWeb::HTTP>(response, SimpleWeb::StatusCode::client_error_bad_request,
                                fail_pair("Out of order pair request (phase 4)"));
      state.remove_pair_cache(cache_key);
      return;
    }
    auto client_cert = x509::cert_from_string(cache.client_cert);
    if (!client_cert) {
      send_xml<SimpleWeb::HTTP>(response, SimpleWeb::StatusCode::client_error_bad_request,
                                fail_pair("Unable to parse client certificate"));
      state.remove_pair_cache(cache_key);
      return;
    }
    auto client_sig = x509::get_cert_signature(client_cert);
    auto public_key = x509::get_cert_public_key(client_cert);
    auto xml = moonlight::pair::client_pair(cache.aes_key, cache.server_challenge.value(),
                                            cache.client_hash.value(), client_secret.value(),
                                            client_sig, public_key);
    bool paired = xml.get<int>("root.paired") == 1;
    send_xml<SimpleWeb::HTTP>(response,
                              paired ? SimpleWeb::StatusCode::success_ok
                                     : SimpleWeb::StatusCode::client_error_bad_request,
                              xml);
    if (paired) {
      state.add_paired_client(cache.client_cert);
      logs::log(logs::info, "Successfully paired {}", client_ip);
    } else {
      logs::log(logs::warning, "Failed pairing with {}", client_ip);
    }
    state.remove_pair_cache(cache_key);
    return;
  }

  logs::log(logs::warning, "Unable to match pair with any phase, retry pairing from Moonlight");
}

void start_http(state::AppState &state) {
  auto server = std::make_shared<HttpServer>();
  server->config.port = state.http_port;
  server->config.address = "0.0.0.0";
  server->default_resource["GET"] = [](auto resp, auto req) {
    send_xml<SimpleWeb::HTTP>(resp, SimpleWeb::StatusCode::client_error_not_found, fail_pair("not found"));
  };

  server->resource["^/serverinfo$"]["GET"] = [&state](auto resp, auto req) {
    serverinfo<SimpleWeb::HTTP>(resp, req, state, false);
  };
  server->resource["^/pair$"]["GET"] = [&state](auto resp, auto req) { http_pair(resp, req, state); };

  server->resource["^/pin/?$"]["GET"] = [](auto resp, auto req) { resp->write(PIN_HTML); };
  server->resource["^/pin/?$"]["POST"] = [](auto resp, auto req) {
    try {
      bt::ptree pt;
      read_json(req->content, pt);
      auto pin = pt.get<std::string>("pin");
      auto secret = pt.get<std::string>("secret");
      std::lock_guard<std::mutex> lk(g_pin_mtx);
      auto it = g_pending_pins.find(secret);
      if (it == g_pending_pins.end()) {
        *resp << "HTTP/1.1 404 Not Found\r\nContent-Length: 14\r\n\r\nunknown secret";
        return;
      }
      it->second.pin->set_value(pin);
      g_pending_pins.erase(it);
      resp->write("OK");
    } catch (const std::exception &e) {
      *resp << "HTTP/1.1 400 Bad Request\r\nContent-Length: " << strlen(e.what()) << "\r\n\r\n" << e.what();
    }
  };

  server->resource["^/unpair$"]["GET"] = [&state](auto resp, auto req) {
    auto headers = req->parse_query_string();
    auto client_id = get_header(headers, "uniqueid");
    auto client_ip = req->remote_endpoint().address().to_string();
    if (client_id) {
      auto cache = state.get_pair_cache(client_id.value() + "@" + client_ip);
      if (cache)
        state.remove_paired_client(cache->client_cert);
    }
    logs::log(logs::info, "Unpair request from {}", client_ip);
    XML xml;
    xml.put("root.<xmlattr>.status_code", 200);
    send_xml<SimpleWeb::HTTP>(resp, SimpleWeb::StatusCode::success_ok, xml);
  };

  logs::log(logs::info, "HTTP server listening on {}", state.http_port);
  server->start();
}

static std::optional<state::PairedClient>
client_if_paired(state::AppState &state, const std::shared_ptr<HttpsServer::Request> &req) {
  return state.get_client_via_ssl(HttpsServer::get_client_cert(req));
}

static void reply_unauthorized(const std::shared_ptr<HttpsServer::Response> &resp,
                               const std::shared_ptr<HttpsServer::Request> &req) {
  logs::log(logs::warning, "HTTPS request from an unpaired client: {}", req->path);
  XML xml;
  xml.put("root.<xmlattr>.status_code", 401);
  xml.put("root.<xmlattr>.query", req->path);
  xml.put("root.<xmlattr>.status_message", "The client is not authorized. Certificate verification failed.");
  send_xml<SimpleWeb::HTTPS>(resp, SimpleWeb::StatusCode::client_error_unauthorized, xml);
}

void start_https(state::AppState &state) {
  auto server = std::make_shared<HttpsServer>(state.cert_path, state.key_path);
  server->config.port = state.https_port;
  server->config.address = "0.0.0.0";
  server->default_resource["GET"] = [](auto resp, auto req) {
    send_xml<SimpleWeb::HTTPS>(resp, SimpleWeb::StatusCode::client_error_not_found, fail_pair("not found"));
  };

  server->resource["^/serverinfo$"]["GET"] = [&state](auto resp, auto req) {
    bool paired = client_if_paired(state, req).has_value();
    if (paired)
      serverinfo<SimpleWeb::HTTPS>(resp, req, state, true);
    else
      reply_unauthorized(resp, req);
  };

  server->resource["^/pair$"]["GET"] = [&state](auto resp, auto req) {
    if (client_if_paired(state, req)) {
      auto headers = req->parse_query_string();
      auto phrase = get_header(headers, "phrase");
      if (phrase && phrase.value() == "pairchallenge") {
        XML xml;
        xml.put("root.paired", 1);
        xml.put("root.<xmlattr>.status_code", 200);
        send_xml<SimpleWeb::HTTPS>(resp, SimpleWeb::StatusCode::success_ok, xml);
      }
    } else {
      reply_unauthorized(resp, req);
    }
  };

  server->resource["^/applist$"]["GET"] = [&state](auto resp, auto req) {
    if (client_if_paired(state, req)) {
      log_req<SimpleWeb::HTTPS>(req);
      send_xml<SimpleWeb::HTTPS>(resp, SimpleWeb::StatusCode::success_ok, moonlight::applist(state.apps));
    } else {
      reply_unauthorized(resp, req);
    }
  };

  auto launch_like = [&state](const std::shared_ptr<HttpsServer::Response> &resp,
                              const std::shared_ptr<HttpsServer::Request> &req, bool resume) {
    if (!client_if_paired(state, req)) {
      reply_unauthorized(resp, req);
      return;
    }
    log_req<SimpleWeb::HTTPS>(req);
    auto headers = req->parse_query_string();
    auto client_ip = req->remote_endpoint().address().to_string();
    auto local_ip = req->local_endpoint().address().to_string();
    auto port = std::to_string(state.rtsp_port);

    std::shared_ptr<session::StreamSession> sess;
    if (resume) {
      sess = state.sessions->get_by_client_ip(client_ip);
      // A resume can arrive from a different device (IP won't match); fall back to the single
      // running session and adopt the new client.
      if (!sess)
        sess = state.sessions->get_running();
      if (!sess) {
        logs::log(logs::warning, "[HTTP] resume with no existing session for {}", client_ip);
        send_xml<SimpleWeb::HTTPS>(resp, SimpleWeb::StatusCode::client_error_bad_request, fail_pair("no session"));
        return;
      }
      if (sess->client_ip != client_ip) {
        logs::log(logs::info, "[HTTP] resume: session {} moving client {} -> {}", sess->session_id,
                  sess->client_ip, client_ip);
        std::lock_guard<std::mutex> lk(*sess->mtx);
        sess->client_ip = client_ip;
      }
      // Resume rotates the per-session AES key; re-capture the new rikey/rikeyid or control +
      // audio keep decrypting with the stale key and flood failures. Don't clobber a working
      // key if the resume omits them.
      auto new_key = get_header(headers, "rikey").value_or("");
      auto new_iv = get_header(headers, "rikeyid").value_or("");
      if (!new_key.empty() && !new_iv.empty()) {
        std::lock_guard<std::mutex> lk(*sess->mtx);
        logs::log(logs::info, "[HTTP] resume session {}: AES key {} (rikey {} -> {}, rikeyid {} -> {})",
                  sess->session_id, sess->aes_key == new_key ? "UNCHANGED" : "ROTATED",
                  sess->aes_key, new_key, sess->aes_iv, new_iv);
        sess->aes_key = new_key;
        sess->aes_iv = new_iv;
      } else {
        logs::log(logs::warning, "[HTTP] resume for session {} missing rikey/rikeyid -- keeping existing key",
                  sess->session_id);
      }
    } else {
      // Already streaming? Re-launching would clobber the live session; resume instead.
      sess = state.sessions->get_by_client_ip(client_ip);
      if (!sess) {
        sess = create_launch_session(state, headers, client_ip);
        state.sessions->add(sess);
        logs::log(logs::info, "[HTTP] launch: created session {} for {} ({}x{}@{}), rtsp_fake_ip={}",
                  sess->session_id, client_ip, sess->client_width, sess->client_height, sess->client_fps,
                  sess->rtsp_fake_ip);
      } else {
        logs::log(logs::info, "[HTTP] launch: client {} already has session {}, resuming", client_ip,
                  sess->session_id);
      }
    }

    // Hand back the per-session fake IP so the RTSP thread can re-identify the session
    // (Moonlight parrots it into the RTSP URI/Host).
    bool use_fake_ip = env_or("STEAM_STREAM_RTSP_FAKE_IP", "TRUE") == "TRUE";
    auto rtsp_host = use_fake_ip ? sess->rtsp_fake_ip : local_ip;
    auto xml = resume ? moonlight::launch_resume(rtsp_host, port) : moonlight::launch_success(rtsp_host, port);
    send_xml<SimpleWeb::HTTPS>(resp, SimpleWeb::StatusCode::success_ok, xml);
  };
  server->resource["^/launch"]["GET"] = [launch_like](auto resp, auto req) { launch_like(resp, req, false); };
  server->resource["^/resume"]["GET"] = [launch_like](auto resp, auto req) { launch_like(resp, req, true); };

  server->resource["^/cancel"]["GET"] = [&state](auto resp, auto req) {
    if (client_if_paired(state, req)) {
      log_req<SimpleWeb::HTTPS>(req);
      auto client_ip = req->remote_endpoint().address().to_string();
      if (auto sess = state.sessions->get_by_client_ip(client_ip)) {
        auto sid = sess->session_id;
        state.sessions->remove(sid);
        if (state.stop_session)
          state.stop_session(sid);
        logs::log(logs::info, "[HTTP] cancel: stopped + removed session {} for {}", sid, client_ip);
      }
      XML xml;
      xml.put("root.<xmlattr>.status_code", 200);
      xml.put("root.cancel", 1);
      send_xml<SimpleWeb::HTTPS>(resp, SimpleWeb::StatusCode::success_ok, xml);
    } else {
      reply_unauthorized(resp, req);
    }
  };

  logs::log(logs::info, "HTTPS server listening on {}", state.https_port);
  server->start();
}

} // namespace HTTPServers
