Adobe Photoshop
文件格式
规格
2019年11月
版权所有 © 1991-2019 Adob​​e Systems Incorporated。保留所有权利。

部分内容版权所有 © 1990-1991 Thomas Knoll。

 
前言
欢迎阅读 Adob​​e Photoshop® 文件格式规范！

本文档详细规定了 Adob​​e Photoshop 文件格式以及 Adob​​e Photoshop 读取和写入的其他相关文件格式。

观众
本文档供第三方读取和写入 Photoshop 原生文件格式。本文档不解释如何解读数据，仅描述数据格式。

本文档包含哪些内容
本文件共分为三章：

Photoshop 文件格式详细描述了 Photoshop PSD和PSB原生文件格式。

其他文档文件格式讨论了 Photoshop 如何处理EPS和TIFF文件格式，Photoshop 也可以创建和读取这些格式。

附加文件格式描述了 Photoshop 用于存储颜色、轮廓、曲线、色阶等信息的其他文件格式。

有关文件格式的更多信息，您可以查阅James D. Murray 和 William vanRyper 编写的《图形文件格式百科全书》（1994 年，O'Reilly & Associates, Inc.，Sebastopol, CA，ISBN 1-56592-058-9）。

本文件中不包含哪些内容
本文档不包含任何关于 2019 年 11 月推出的 PSDC（Photoshop 云文档）的信息。目前，该格式为私有格式。点击此链接可了解更多关于Photoshop 云文档的信息。

SDK 用户论坛
Adobe 论坛网页（http://www.adobe.com/support/forums）也可用于讨论 SDK 问题。从上述页面，点击 Photoshop 链接，然后点击 Adob​​e Photoshop Developers 链接。



内容
前言
观众

本文档包含哪些内容

本文件中不包含哪些内容

SDK 用户论坛

Photoshop 文件格式
介绍

大型文档格式

视窗

Mac OS

Photoshop 文件格式

文件头部分

颜色模式数据部分

图片资源部分

图像资源块

图像资源 ID

层和掩模信息部分

附加图层信息

图像数据部分

其他文档文件格式
Photoshop EPS 文件

TIFF 文件

Photoshop 特有的 TIFF 标签

Mac OS 上的 TIFF 文件

其他文件格式
行动

任意地图

单通道活动通道

CMYK 设置

彩色书

颜色表

色卡

轮廓

曲线

自定义内核

双色调选项

半色调网屏

色相/饱和度

级别

显示器设置

替换颜色/颜色范围

选择性颜色

分离表

传递函数

 
Photoshop 文件格式
介绍
本章讨论 Photoshop 的原生文件格式

Photoshop 文件类型
你

文件类型/扩展名

Mac OS

8BPS

视窗

.PSD

大型文档格式
大型文档格式 (8BPB/PSB) 支持任意维度最大可达 30 万像素的文档。所有 Photoshop 功能，例如图层、效果和滤镜，都受 PSB 格式支持。PSB 格式在许多方面与 Photoshop 原生格式相同。本文档将通过添加 **PSB** 标记来介绍 PSB 格式的差异。

视窗
所有数据均以大端字节序存储。在 Windows 平台上，读写​​时必须交换短整型和长整型的字节顺序。

Mac OS
为了实现跨平台兼容性，Photoshop 所需的所有信息都存储在数据分支中。但是，为了与其他 Macintosh 应用程序互操作，某些信息会在资源分支中重复存储：

为了与图像编目应用程序兼容，“pnot”资源ID 0包含对存储在其他资源中的缩略图、关键字和标题信息的引用。

缩略图存储在“PICT”资源中，关键字存储在“STR#”资源128中，标题文本存储在“TEXT”资源128中。有关这些资源格式的更多信息，请参阅《Inside Macintosh: QuickTime Components and the Extensis Fetch Awareness Developer's Toolkit》。

Photoshop 还会创建“icl8” -16455 和“ICN#” -16455 资源，其中包含缩略图，这些缩略图将显示在 Mac OS Finder 中。

Photoshop 文件信息对话框中的所有数据都存储在“ANPA”资源 10000 中。此资源中的数据以 IPTC-NAA 记录 2 的形式存储。有关此资源格式的更多信息，请参阅“文档”文件夹中的IPTC文件夹中的文档。

Photoshop 文件格式
Photoshop 文件格式分为五个主要部分，如Photoshop 文件结构图所示。Photoshop 文件格式包含许多长度标记。使用这些长度标记可以在不同部分之间跳转。长度标记通常会用字节进行填充，以便四舍五入到最接近的 2 字节或 4 字节的间隔。

Photoshop 文件结构
Photoshop 文件格式
文件头（文件头部分）。

颜色模式数据（颜色模式数据部分）

图片资源（图片资源部分）

图层和掩模信息（图层和掩模信息部分）

图像数据（图像数据部分）。

文件头部分长度固定；其他四个部分长度可变。

写入这些节时，应写入该节中的所有字段，因为 Photoshop 可能会尝试读取整个节。写入文件时，如果跳过某些字节，则应将跳过的字段显式地写入零。

读取以长度分隔符分隔的节时，请使用长度字段来决定何时停止读取。在大多数情况下，长度字段指示的是后续的字节数，而不是记录数。

所有表中“长度”列的值均以字节为单位。

所有定义为Unicode 字符串的值包含：

一个 4 字节的长度字段，表示字符串中 UTF-16 代码单元的数量（不是字节数）。

Unicode 值字符串，每个字符占用两个字节，字符串末尾用两个字节的空字符表示。

文件头部分
文件头包含图像的基本属性。

文件头部分
长度

描述

4

签名：始终等于“8BPS”。如果签名与此值不匹配，请勿尝试读取文件。

2

版本：始终等于 1。如果版本与此值不匹配，请勿尝试读取文件。（**PSB** 版本为 2。）

6

保留值：必须为零。

2

图像中的通道数，包括所有 alpha 通道。支持的范围为 1 到 56。

4

图像高度（以像素为单位）。支持的范围为 1 到 30,000。

（**PSB** 最高限额为 300,000。）

4

图像宽度，以像素为单位。支持的范围为 1 到 30,000。

（*PSB** 最高 300,000）

2

深度：每个通道的位数。支持的值为 1、8、16 和 32。

2

文件的颜色模式。支持的值有：位图 = 0；灰度 = 1；索引 = 2；RGB = 3；CMYK = 4；多通道 = 7；双色调 = 8；Lab = 9。

颜色模式数据部分
颜色模式数据部分结构如下：

颜色模式数据部分
长度

描述

4

以下颜色数据的长度。

多变的

颜色数据。

只有索引颜色和双色调（参见文件头部分的模式字段）才包含颜色模式数据。对于所有其他模式，此部分仅包含 4 字节的长度字段，其值为零。

索引彩色图像：长度为 768；颜色数据包含图像的颜色表，按非交错顺序排列。

双色调图像：颜色数据包含双色调规范（其格式未公开）。其他读取 Photoshop 文件的应用程序可以将双色调图像视为灰度图像，并在读写文件时仅保留双色调信息的内容。

图片资源部分
文件的第三部分包含图像资源。它以长度字段开头，后面跟着一系列资源块。

图片资源部分
长度

描述

4

图片资源部分的长度。长度可以为零。

多变的

图像资源（图像资源块）。

图像资源块
图像资源块是多种文件格式的基本构建单元，包括 Photoshop 的原生文件格式、JPEG 和 TIFF。图像资源用于存储与图像相关的非像素数据，例如钢笔工具路径。

它们被称为资源块，因为它们保存着早期 Photoshop 版本中 Macintosh 资源分支中存储的数据。

图像资源块的基本结构如图所示。最后一个字段是数据区，其大小因资源类型而异。每种资源类型的组成将在以下章节中进行描述。

图像资源块
长度

描述

4

签名：'8BIM'

2

资源的唯一标识符。图像资源 ID包含 Photoshop 使用的资源 ID 列表。

多变的

名称：Pascal 字符串，填充以使长度为偶数（空名称由两个字节的 0 组成）

4

接下来是资源数据的实际大小

多变的

资源数据在各个资源类型章节中进行了描述。为了保证数据大小均匀，数据已进行填充。

图像资源 ID
图像资源使用几个标准 ID 号，如“图像资源 ID”部分所示。并非所有文件格式都使用所有 ID。某些信息可能存储在文件的其他部分。

对于自 Photoshop 3.0 以来添加的资源 ID，该条目指示了引入它们的版本，例如（Photoshop 6.0）。

图像资源 ID
ID

描述

十六进制

小数

0x03E8

1000

（已过时——仅适用于 Photoshop 2.0）包含五个 2 字节值：通道数、行数、列数、深度和模式

0x03E9

1001

Macintosh 打印管理器打印信息记录

0x03EA

1002

Macintosh 页面格式信息。Photoshop 已不再读取。（已过时）

0x03EB

1003

（已过时——仅适用于 Photoshop 2.0）索引颜色表

0x03ED

1005

ResolutionInfo结构。请参阅Photoshop API 指南.pdf中的附录 A。

0x03EE

1006

alpha 通道的名称以一系列 Pascal 字符串的形式表示。

0x03EF

1007

（已过时）参见 ID 1077 DisplayInfo结构。参见Photoshop API 指南.pdf中的附录 A。

0x03F0

1008

标题以 Pascal 字符串的形式呈现。

0x03F1

1009

边框信息。包含一个固定数字（2 字节实数，2 字节小数）表示边框宽度，以及 2 字节表示边框单位（1 = 英寸，2 = 厘米，3 = 磅，4 = 派卡，5 = 列）。

0x03F2

1010

背景颜色。参见颜色结构。

0x03F3

1011

打印标志。一系列单字节布尔值（请参阅“页面设置”对话框）：标签、裁剪标记、颜色条、套准标记、负片、翻转、插值、标题、打印标志。

0x03F4

1012

灰度和多通道半色调信息

0x03F5

1013

颜色半色调信息

0x03F6

1014

双色调半色调信息

0x03F7

1015

灰度和多通道传递函数

0x03F8

1016

颜色传递函数

0x03F9

1017

双音传递函数

0x03FA

1018

双色调图像信息

0x03FB

1019

两个字节分别表示点范围的有效黑白值

0x03FC

1020

（过时的）

0x03FD

1021

EPS期权

0x03FE

1022

快速掩码信息。2 字节包含快速掩码通道 ID；1 字节布尔值，指示掩码最初是否为空。

0x03FF

1023

（过时的）

0x0400

1024

层状态信息。2 字节，包含目标层的索引（0 = 底层）。

0x0401

1025

工作路径（未保存）。请参阅“路径资源格式”。

0x0402

1026

图层组信息。每个图层占用 2 个字节，包含拖拽组的组 ID。同一组中的图层具有相同的组 ID。

0x0403

1027

（过时的）

0x0404

1028

IPTC-NAA 记录。包含文件信息……详情请参阅“文档”文件夹中IPTC文件夹内的文档。

0x0405

1029

原始格式文件的图像模式

0x0406

1030

JPEG 画质。私密。

0x0408

1032

（Photoshop 4.0）网格和参考线信息。请参阅“网格和参考线资源格式”。

0x0409

1033

（Photoshop 4.0）仅适用于 Photoshop 4.0 的缩略图资源。请参阅缩略图资源格式。

0x040A

1034

（Photoshop 4.0）版权标记。布尔值，指示图像是否受版权保护。可通过“属性”套件设置，也可由用户在“文件信息”中设置……

0x040B

1035

（Photoshop 4.0） URL。包含统一资源定位符的文本字符串句柄。可通过“属性”套件设置，也可由用户在“文件信息”中设置。

0x040C

1036

（Photoshop 5.0）缩略图资源（取代资源 1033）。请参阅缩略图资源格式。

0x040D

1037

（Photoshop 5.0）全局角度。4 字节，包含一个介于 0 和 359 之间的整数，表示效果图层的全局光照角度。如果未指定，则默认为 30。

0x040E

1038

（已过时）请参阅下方的 ID 1073。（Photoshop 5.0）颜色样本资源。请参阅颜色样本资源格式。

0x040F

1039

（Photoshop 5.0） ICC配置文件。ICC（国际色彩联盟）格式配置文件的原始字节。请参阅“文档”文件夹中的ICC1v42_2006-05.pdf和“示例代码\Common\Includes”中的icProfileHeader.h。

0x0410

1040

（Photoshop 5.0）水印。1 字节。

0x0411

1041

（Photoshop 5.0） ICC 未标记配置文件。1 字节，用于禁用打开文件时任何假定的配置文件处理。1 = 故意不标记。

0x0412

1042

（Photoshop 5.0）效果可见。一个 1 字节的全局标志，用于显示/隐藏所有效果图层。仅当效果图层被隐藏时才显示。

0x0413

1043

（Photoshop 5.0）专色半色调。版本信息占 4 字节，长度信息占 4 字节，以及可变长度数据。

0x0414

1044

（Photoshop 5.0）文档特定的 ID 种子编号。4 字节：基准值，用于生成图层 ID（如果现有 ID 已超过此值，则使用更大的值）。其目的是避免出现添加图层、拼合图层、保存、重新打开，然后再添加更多图层，最终生成与第一组 ID 相同的图层的情况。

0x0415

1045

（Photoshop 5.0） Unicode 字母名称。Unicode字符串

0x0416

1046

（Photoshop 6.0）索引颜色表计数。2 字节，用于存储表中实际定义的颜色数量。

0x0417

1047

（Photoshop 6.0）透明度索引。2 字节，用于存储透明颜色的索引（如果有）。

0x0419

1049

（Photoshop 6.0）全局海拔高度。海拔高度的 4 字节条目。

0x041A

1050

（Photoshop 6.0）切片。请参阅切片资源格式。

0x041B

1051

（Photoshop 6.0）工作流程 URL。Unicode字符串

0x041C

1052

（Photoshop 6.0）跳转到 XPEP。2 字节主版本号，2 字节次版本号，4 字节计数。计数重复以下内容：4 字节块大小，4 字节键值，如果键值为'jtDd'，则下一个字节为脏标记的布尔值；否则，下一个字节为修改日期。

0x041D

1053

（Photoshop 6.0） Alpha 标识符。长度为 4 字节，后面跟着每个 Alpha 标识符的 4 个字节。

0x041E

1054

（Photoshop 6.0） URL 列表。4 字节的 URL 计数，后跟 4 字节长的 URL、4 字节的 ID 和每个计数对应的Unicode 字符串。

0x0421

1057

（Photoshop 6.0）版本信息。4 字节版本，1 字节hasRealMergedData，Unicode 字符串：写入器名称，Unicode 字符串：读取器名称，4 字节文件版本。

0x0422

1058

（Photoshop 7.0） EXIF 数据 1. 请参阅http://www.kodak.com/global/plugins/acrobat/en/service/digCam/exifStandard2.pdf

0x0423

1059

（Photoshop 7.0） EXIF 数据 3. 请参阅http://www.kodak.com/global/plugins/acrobat/en/service/digCam/exifStandard2.pdf

0x0424

1060

（Photoshop 7.0） XMP 元数据。文件信息以 XML 描述形式呈现。请参阅http://www.adobe.com/devnet/xmp/

0x0425

1061

（Photoshop 7.0）标题摘要。16 字节：RSA 数据安全，MD5 消息摘要算法

0x0426

1062

（Photoshop 7.0）打印比例。2 字节样式（0 = 居中，1 = 适应尺寸，2 = 用户自定义）。4 字节 x 坐标（浮点数）。4 字节 y 坐标（浮点数）。4 字节缩放比例（浮点数）。

0x0428

1064

（Photoshop CS）像素宽高比。4 字节（版本 = 1 或 2），8 字节双精度浮点数，像素的 x/y 坐标。版本 2 尝试修正 NTSC 和 PAL 制式的值，之前偏差约为 5%。

0x0429

1065

（Photoshop CS）图层复合。4 字节（描述符版本 = 16），描述符（参见描述符结构）

0x042A

1066

（Photoshop CS）备选双色调颜色。2 字节（版本 = 1），2 字节计数，每个计数重复以下内容：[颜色：2 字节空格，后跟 4 个 2 字节颜色分量]，之后是另一个 2 字节计数，通常为 256，再之后是 Lab 颜色，L、a、b 各占一个字节。Photoshop 不读取或使用此资源。

0x042B

1067

（Photoshop CS）备选专色。2 字节（版本 = 1），2 字节通道计数，每个计数重复以下内容：4 字节通道 ID，颜色：2 字节空格，后跟 4 个 2 字节颜色分量。Photoshop 不会读取或使用此资源。

0x042D

1069

（Photoshop CS2）图层选择 ID。2 字节计数，以下计数重复出现：4 字节图层 ID

0x042E

1070

（Photoshop CS2） HDR色调信息

0x042F

1071

（Photoshop CS2）打印信息

0x0430

1072

（Photoshop CS2）图层组启用 ID。文档中每个图层占用 1 个字节，重复次数为资源长度。注意：图层组具有起始标记和结束标记。

0x0431

1073

（Photoshop CS3）颜色取样器资源。另请参阅 ID 1038 了解旧格式。请参阅“颜色取样器资源格式”。

0x0432

1074

（Photoshop CS3）测量标尺。4 字节（描述符版本 = 16），描述符（参见描述符结构）

0x0433

1075

（Photoshop CS3）时间线信息。4 字节（描述符版本 = 16），描述符（参见描述符结构）

0x0434

1076

（Photoshop CS3）工作表披露。4 字节（描述符版本 = 16），描述符（参见描述符结构）

0x0435

1077

（Photoshop CS3）用于支持浮点颜色的DisplayInfo结构。另请参阅 ID 1007。请参阅Photoshop API 指南.pdf中的附录 A。

0x0436

1078

（Photoshop CS3）洋葱皮。4 字节（描述符版本 = 16），描述符（参见描述符结构）

0x0438

1080

（Photoshop CS4）计数信息。4 字节（描述符版本 = 16），描述符（参见描述符结构）文档中计数的相关信息。参见计数工具。

0x043A

1082

（Photoshop CS5）打印信息。4 字节（描述符版本 = 16），描述符（参见描述符结构）。文档中当前打印设置的信息。颜色管理选项。

0x043B

1083

（Photoshop CS5）打印样式。4 字节（描述符版本 = 16），描述符（参见描述符结构）。文档中当前打印样式的信息。打印标记、标签、装饰等。

0x043C

1084

（Photoshop CS5） Macintosh NSPrintInfo。Macintosh 操作系统特有的可变信息。NSPrintInfo。建议您不要解读或使用此数据。

0x043D

1085

（Photoshop CS5） Windows DEVMODE。适用于 Windows 的可变操作系统特定信息。DEVMODE。建议您不要解读或使用此数据。

0x043E

1086

（Photoshop CS6）自动保存文件路径。Unicode字符串。建议您不要解读或使用此数据。

0x043F

1087

（Photoshop CS6）自动保存格式。Unicode字符串。建议您不要解读或使用此数据。

0x0440

1088

（Photoshop CC）路径选择状态。4 字节（描述符版本 = 16），描述符（参见描述符结构）有关当前路径选择状态的信息。

0x07D0-0x0BB6

2000-2997

路径信息（已保存的路径）。请参阅“路径资源格式”。

0x0BB7

2999

剪切路径的名称。请参阅“路径资源格式”。

0x0BB8

3000

（Photoshop CC）原始路径信息。4 字节（描述符版本 = 16），描述符（参见描述符结构）有关原始路径数据的信息。

0x0FA0-0x1387

4000-4999

插件资源。由插件添加的资源。请参阅 SDK 文档中的插件 API。

0x1B58

7000

图像就绪变量。变量定义的 XML 表示

0x1B59

7001

图像就绪数据集

0x1B5A

7002

图像就绪默认选中状态

0x1B5B

7003

Image Ready 7 鼠标悬停展开状态

0x1B5C

7004

图像就绪鼠标悬停展开状态

0x1B5D

7005

图像准备就绪，保存图层设置

0x1B5E

7006

图像就绪版本

0x1F40

8000

（Photoshop CS3） Lightroom 工作流程，如果存在，则表示该文档处于 Lightroom 工作流程中。

0x2710

10000

打印标志信息。2 字节版本（= 1），1 字节中心裁剪标记，1 字节（= 0），4 字节出血宽度值，2 字节出血宽度比例。

以下各节将更详细地介绍一些资源格式。

网格和指南资源格式

Photoshop 将图像的网格和参考线信息存储在图像资源块中。每个资源块由一个始终存在的初始 16 字节网格和参考线标头组成，后面跟着 5 字节的特定参考线信息块，用于指示参考线的方向和位置，这些信息块仅在存在参考线时存在（fGuideCount > 0）。

网格和指南标题
长度

描述

4

版本（= 1）

8

未来将实现文档专属网格（水平方向 4 字节，垂直方向 4 字节）。目前，网格周期设置为每四分之一英寸，即水平和垂直方向均为 576（在 72 dpi 下，即 18 * 32 = 576）。

4

fGuideCount：指南资源块的数量（可以为 0）。

 
指南资源块
长度

描述

4

参考线在文档坐标系中的位置。由于参考线要么是垂直的，要么是水平的，因此只需要坐标系的一个分量即可。

1

导向方向。VHSelect 是一个系统类型，类型为无符号字符，其中 0 = 垂直，1 = 水平。

可以使用“属性”套件修改网格和参考线信息。有关更多信息，请参阅Photoshop API 指南.pdf中的“回调”章节。

缩略图资源格式

Adobe Photoshop（5.0 及更高版本）将用于预览显示的缩略图信息存储在图像资源块中，该块由一个初始的 28 字节标头组成，后跟一个按 RGB（红、绿、蓝）顺序排列的 JFIF 缩略图，适用于 Macintosh 和 Windows。

Adobe Photoshop 4.0 以相同的格式存储缩略图信息，只是数据部分采用 BGR（蓝、绿、红）颜色编码。4.0 格式的资源 ID 为 1033，5.0 格式的资源 ID 为 1036。

缩略图资源标题
长度

描述

4

格式。1 = kJpegRGB。也支持 kRawRGB (0)。

4

缩略图宽度（像素）。

4

缩略图高度（像素）。

4

宽度字节：填充行字节 = (宽度 * 每像素位数 + 31) / 32 * 4。

4

总大小 = 宽度（字节）* 高度 * 平面数

4

压缩后的尺寸。用于一致性检查。

2

每像素位数 = 24

2

飞机数量 = 1

多变的

RGB格式的JFIF数据。

资源 ID 1033 的数据格式为 BGR。

颜色采样器资源格式

Adobe Photoshop（5.0 及更高版本）将图像的颜色采样器信息存储在图像资源块中，该块由一个初始的 8 字节颜色采样器标头和一个可变长度的特定颜色采样器信息块组成。

颜色采样器标题
长度

描述

4

版本（= 1、2 或 3）

4

后续将提供多个颜色样本。请参阅“查看颜色样本”资源块。

 
颜色采样器资源块
长度

描述

4

颜色采样器版本，版本 3 为 1。（仅限版本 3）。

8

点的水平和垂直位置（各占 4 字节）。版本 1 为固定值。版本 2 为浮点值。

2

颜色空间：枚举 { colorCodeDummy = -1, RGB, HSB, CMYK, Pantone, Focoltone, Trumatch, Toyo, Lab, Gray, WideCMYK, HKS, DIC, TotalInk, MonitorRGB, Duotone, Opacity, Web, GrayFloat, RGBFloat, OpacityFloat};

2

深度（仅限版本 2）

路径资源格式

Photoshop 将图像保存的路径存储在图像资源块中。这些资源块由一系列 26 字节的路径点记录组成，因此资源长度必须始终是 26 的倍数。

Photoshop 将路径存储为8BIM类型的资源，ID 范围为 2000 到 2997。这些编号应保留给 Photoshop 使用。资源名称是保存路径时为其指定的名称。

如果文件包含类型为8BIM、 ID 为 2999 的资源，则该资源包含一个 Pascal 风格的字符串，其中包含将此图像另存为 EPS 文件时要使用的剪切路径名称。平面度为 4 字节固定值，填充规则为 2 字节。0 表示相同填充规则，1 表示奇偶填充规则，2 表示非零缠绕填充规则。Photoshop 会忽略填充规则。

GetProperty()调用返回的路径格式与下述格式相同。请参阅IllustratorExport示例插件代码，了解此资源数据的构建方式。

路径点

用于定义路径的所有点都以 8 个字节的形式存储，作为一对 32 位分量，垂直分量在前。

这两个分量均为有符号定点数，二进制小数点前有 8 位，小数点后有 24 位。小数点后预留了三个保护位，以消除大部分算术溢出的风险。因此，每个分量的取值范围为0xF0000000到0x0FFFFFFF，代表 -16 到 16 的范围。下限包含在内，但上限不包含在内。

之所以使用这个有限的范围，是因为这些点是相对于图像尺寸而言的。垂直分量是相对于图像高度而言的，水平分量是相对于图像宽度而言的。[ 0,0 ] 表示图像的左上角；[ 1,1 ]（[ 0x01000000,0x01000000 ]）表示右下角。

在 Windows 系统中，路径点组件的字节顺序是相反的；访问每个 32 位值时，应该交换字节顺序。

路径记录

路径资源中的数据由一个或多个 26 字节的记录组成。每个记录的前两个字节是选择器，用于指示路径类型。在 Windows 系统中，访问路径时需要交换这两个字节，才能将其作为短路径访问。

路径数据记录类型
选择器

描述

0

闭合子路径长度记录

1

闭合子路径贝塞尔结，链接

2

闭合子路径贝塞尔结，未连接

3

打开子路径长度记录

4

开放子路径贝塞尔结，链接

5

开放子路径贝塞尔结，未连接

6

路径填充规则记录

7

剪贴板记录

8

初始填充规则记录

第一个 26 字节的路径记录包含一个选择器值 6，即路径填充规则记录。第一个记录的其余 24 个字节为零。路径采用奇偶规则。子路径长度记录的选择器值为 0 或 3，其第 2 和第 3 个字节包含贝塞尔节点记录的数量。剩余的 22 个字节未使用，应为零。每个长度记录之后紧跟描述子路径节点的贝塞尔节点记录。

在贝塞尔结记录中，选择器字段之后的 24 个字节包含三个路径点（如上所述）：

贝塞尔曲线段中位于节点之前的控制点

绳结的锚点，以及

离开节点的贝塞尔线段的控制点。

已链接的节点其控制点相互链接。编辑其中一个点会同时修改其他点，以保持共线。只有当节点的控制点与其锚点共线时，才应将其标记为具有链接控制点。未链接节点上的控制点彼此独立。更多信息，请参阅Adob​​e Photoshop 用户指南。

剪贴板记录，选择器=7，包含四个定点数表示边界矩形（上、左、下、右），以及一个表示分辨率的定点数。

初始填充记录（选择器=8）包含一个双字节记录。值为 1 表示填充从所有像素开始。该值可以是 0 或 1。

切片资源格式

Adobe Photoshop 6.0 将图像的切片信息存储在图像资源块中。

Adobe Photoshop 7.0 在块的末尾添加了一个描述符，用于显示各个切片的信息。

Adobe Photoshop CS 及后续版本（7 或 8）使用描述符来定义切片数据。

版本 7 或 8 的切片标题
长度

描述

4

版本（= 7 和 8）

4

描述符版本（= Photoshop 6.0 为 16）。

多变的

描述符（参见描述符结构）

版本 6 的切片标题
长度

描述

4

版本（= 6）

4 * 4

所有切片的边界矩形：所有切片的上、左、下、右边界矩形

多变的

切片组名称：Unicode 字符串

4

接下来要处理的切片数量。请参见下表中的“切片”资源块。

 
切片资源块
长度

描述

4

ID

4

组 ID

4

起源

4

关联图层 ID

仅当 Origin = 1 时存在

多变的

名称：Unicode字符串

4

类型

4 * 4

左侧、顶部、右侧、底部位置

多变的

URL：Unicode字符串

多变的

目标：Unicode字符串

多变的

消息：Unicode 字符串

多变的

Alt 标签：Unicode 字符串

1

单元格文本是 HTML：布尔值

多变的

单元格文本：Unicode 字符串

4

水平对齐

4

垂直对齐

1

Alpha 颜色

1

红色的

1

绿色的

1

蓝色的

篇幅允许的情况下可添加更多数据。参见上文注释。

4

描述符版本（= Photoshop 6.0 为 16）。

多变的

描述符（参见描述符结构）

 

消失点资源格式

Adobe Photoshop CS2 (9.0) 及更高版本会将图像的消失点信息存储在图像资源块中。整个资源是一个字符串，在 Windows 系统中其 ID 为“tnaF”，在 Macintosh 系统中其 ID 为“FaNt”。资源结构如下：

词汇：

关系——一组相关的平面。

根平面——关系中的第一个平面。

校准顺序 - 从根平面开始，对关系中的平面进行排序，深度优先，递归遍历与给定平面相连的平面。

基础知识：

平面区域由一系列消失射线构成。射线定义了平面区域的一条虚拟边。射线的结构记录了撕裂和方向问题所需的信息。平行射线必须指向同一个虚拟点 (VPID)。主射线的原点表示平面上距离两个虚拟点最远的点。两条主射线共享一个原点。

版本 = 101

需要关注的关系数量。

——对于每个关系——

根平面的网格分辨率

要跟踪的飞机数量

——按校准顺序对每个平面进行校准——

飞机识别码

用于校准此平面的平面的 ID，如果没有则为 0。

——针对4条射线——

射线的起始位置。点

VP 位置 - 除非是端点，否则在关系中的所有平面上必须保持一致。点

如果 VP 位置是端点，则为真

该射线指向的ID。

Ray DI（见下文）

 

 

++++++++++++++++++++

I/O 附录

点 - 两个双打；h endl，v endl

VPID - 整数（枚举值）0、1、2，标识 3 个可能的虚拟处理器之一

RayID - 1，直接连接到共享原点的主射线之一。

3，一条与 7 平行的非主射线

5，一条与 1 平行的非主射线

7、与共享原点直接相连的主射线之一。

层和掩模信息部分
Photoshop 文件的第四部分包含有关图层和蒙版的信息。本文档的这一部分描述了图层和蒙版记录的格式。

完整的合并图像数据并未存储在此处。完整的合并/合成图像位于文件的最后一个部分。请参阅“图像数据部分”。如果未选中“最大兼容性”选项，则不会创建合并/合成图像，必须读取图层数据才能重现最终图像。

请参阅“图层和掩码信息”部分，其中显示了此部分的整体结构。如果没有图层或掩码，则此部分仅包含 4 个字节：长度字段，其值为零。（**PSB** 长度为 8 字节）

“Layr”、“Lr16”和“Lr32”的起始位置请参见图层信息。（注：该部分的长度可能已知。）

分析本部分时，请密切注意各部分的长度。

图层和掩模信息部分
长度

描述

4

图层和掩码信息部分的长度。（**PSB** 长度为 8 字节。）

多变的

图层信息（详情请参见图层信息）。

多变的

全局图层掩码信息（详情请参阅“全局图层掩码信息”）。

多变的

（Photoshop 4.0 及更高版本）

一系列带有标签的数据块，其中包含各种类型的数据。有关可包含在此处的数据类型列表，请参阅“附加图层信息” 。

“图层信息”显示了图层信息的高级组织结构。

图层信息
长度

描述

4

图层信息部分的长度，向上取整到 2 的倍数。（**PSB** 长度为 8 字节。）

2

图层数。如果为负数，则其绝对值即为图层数，第一个 alpha 通道包含合并结果的透明度数据。

多变的

各层的信息。请参阅“层记录”部分，其中描述了各层的信息结构。

多变的

通道图像数据。包含每一层的一个或多个图像数据记录（结构请参见“通道图像数据”）。层顺序与层信息（本表的前一行）中的顺序相同。

 
层记录
长度

描述

4 * 4

包含图层内容的矩形区域。由上、左、下、右坐标指定。

2

层中的通道数

6 *

通道数量

通道信息。每个通道六个字节，包括：

通道 ID 为 2 字节：0 = 红色，1 = 绿色，等等；

-1 = 透明蒙版；-2 = 用户提供的图层蒙版；-3 = 用户实际提供的图层蒙版（当同时存在用户提供的蒙版和矢量蒙版时）

对应通道数据的长度为 4 字节。（**PSB** 对应通道数据的长度为 8 字节。）有关通道数据的结构，请参阅“通道图像数据” 。

4

混合模式签名：'8BIM'

4

混合模式键：


'pass' = 直通，'norm' = 正常，'diss' = 溶解，'dark' = 变暗，'mul' = 乘，'idiv' = 颜色加深，'lbrn' = 线性加深，'dkCl' = 更暗的颜色，'lite' = 变亮，'scrn' = 滤色，'div' = 颜色减淡，'lddg' = 线性减淡，'lgCl' = 更亮的颜色，'over' = 叠加，'sLit' = 柔光，'hLit' = 强光，'vLit' = 鲜艳光，'lLit' = 线性光，'pLit' = 点光源，'hMix' = 强混合，'diff' = 差值，'smud' = 排除，'fsub' = 减去，'fdiv' = 除法，'hue' = 色调，'sat' =饱和度，'colr' = 颜色，'lum' = 亮度，

1

不透明度。0 = 透明……255 = 不透明

1

裁剪：0 = 基部，1 = 非基部

1

标志：
位 0 = 透明度受保护；
位 1 = 可见；
位 2 = 已过时；
位 3 = 1（适用于 Photoshop 5.0 及更高版本），指示位 4 是否包含有用信息；
位 4 = 与文档外观无关的像素数据

1

填充（零）

4

额外数据字段的长度（=接下来五个字段的总长度）。

多变的

图层掩码数据：结构请参见“图层掩码/调整图层数据” 。可以是 40 字节、24 字节，或者如果没有图层掩码，则为 4 字节。

多变的

图层混合范围：请参阅图层混合范围数据。

多变的

层名称：Pascal 字符串，填充至 4 字节的倍数。

图层蒙版/调整图层数据
长度

姓名

4

数据大小：检查数据大小和标志以确定哪些数据存在或不存在。如果为零，则表示以下字段不存在。

4 * 4

包含图层蒙版的矩形：上、左、下、右

1

默认颜色。0 或 255

1

旗帜。

位 0 = 相对于图层的位置
；位 1 = 图层蒙版禁用；
位 2 = 混合时反转图层蒙版（已弃用）
；位 3 = 指示用户蒙版实际上来自渲染其他数据；
位 4 = 指示用户蒙版和/或矢量蒙版应用了参数。

1

掩码参数。仅当上述标志的第 4 位设置时才存在。

多变的

掩码参数位标志如下：
位 0 = 用户掩码密度，1 字节；
位 1 = 用户掩码羽化，8 字节，双精度浮点数；
位 2 = 矢量掩码密度，1 字节；
位 3 = 矢量掩码羽化，8 字节，双精度浮点数。

2

填充。仅当 size = 20 时存在。否则，存在以下内容。

1

真实国旗。与上述国旗信息相同。

1

真实用户面具背景。0 或 255。

4 * 4

包围图层蒙版的矩形：上、左、下、右。

图层混合范围数据
长度

姓名

4

图层混合范围数据的长度

4

复合灰度混合源。包含 2 个黑色值​​和 2 个白色值。存在，但与 Lab 和灰度模式无关。

4

复合灰混合目的地范围

4

第一通道源范围

4

第一频道目的地范围

4

第二通道源范围

4

第二通道目的地范围

...

...

4

第N通道源范围

4

第N个信道目的地范围

通道图像数据
长度

描述

2

压缩。0 = 原始数据，1 = RLE 压缩，2 = 无预测的 ZIP，3 = 带预测的 ZIP。

多变的

图像数据。

如果压缩代码为 0，则图像数据只是原始图像数据，其大小计算为（LayerBottom-LayerTop）*（LayerRight-LayerLeft） （来自See Layer 记录中的第一个字段）。

如果压缩码为 1，则图像数据首先是通道中所有扫描线（从底层到顶层）的字节计数，每个计数存储为一个双字节值。（**PSB** 模式下，每个计数存储为一个四字节值。）接下来是 RLE 压缩数据，每条扫描线单独压缩。RLE 压缩算法与 Macintosh ROM 例程 PackBits 和 TIFF 标准使用的压缩算法相同。

如果图层的大小（因此数据量）为奇数，则会在该行的末尾插入一个填充字节。

如果该图层是调整图层，则通道数据未定义（可能全部为白色）。

全局图层掩码信息
长度

描述

4

全局图层掩码信息部分的长度。

2

叠加颜色空间（未记录）。

8

4 * 2 字节颜色分量

2

不透明度。0 = 透明，100 = 不透明。

1

类型。0 = 已选择颜色（即反转）；1 = 已保护颜色；128 = 使用每层存储的值。此值为首选。其他值用于向后兼容测试版。

多变的

填充：零

附加图层信息
Photoshop 4.0 及更高版本中新增了几种图层信息。这些信息位于图层记录结构的末尾（参见“图层记录”的最后一行）。它们的结构如下：

附加图层信息
长度

描述

4

签名：'8BIM'或'8B64'

4

图例：一个 4 位字符代码（参见各章节）

4

以下长度数据已向上取整至偶数字节数。

(**PSB**, the following keys have a length count of 8 bytes: LMsk, Lr16, Lr32, Layr, Mt16, Mt32, Mtrn, Alph, FMsk, lnk2, FEid, FXid, PxSD.

Variable

Data (See individual sections)

The following sections describe the different types of data available, their keys and their format.

Adjustment layer (Photoshop 4.0)

Adjustment layers can have one of the following keys:

'SoCo' = Solid Color

'GdFl' = Gradient

'PtFl' = Pattern

'brit' = Brightness/Contrast

'levl' = Levels

'curv' = Curves

'expA' = Exposure

'vibA' = Vibrance

'hue ' = Old Hue/saturation, Photoshop 4.0

'hue2' = New Hue/saturation, Photoshop 5.0

'blnc' = Color Balance

'blwh' = Black and White

'phfl' = Photo Filter

'mixr' = Channel Mixer

'clrL' = Color Lookup

'nvrt' = Invert

'post' = Posterize

'thrs' = Threshold

'grdm' = Gradient Map

'selc' = Selective color

 

The data for the adjustment layer is the same as the load file formats for each format. See See Additional File Formats for information.

Effects Layer (Photoshop 5.0)

The key for the effects layer is 'lrFX' . The data has the following format:

Effects Layer info
Length

Description

2

Version: 0

2

Effects count: may be 6 (for the 6 effects in Photoshop 5 and 6) or 7 (for Photoshop 7.0)

The next three items are repeated for each of the effects.

4

Signature: '8BIM'

4

Effects signatures: OSType key for which effects type to use:

'cmnS' = common state (see See Effects layer, common state info)

'dsdw' = drop shadow (see See Effects layer, drop shadow and inner shadow info)

'isdw' = inner shadow (see See Effects layer, drop shadow and inner shadow info)

'oglw' = outer glow (see See Effects layer, outer glow info)

'iglw' = inner glow (see See Effects layer, inner glow info)

'bevl' = bevel (see See Effects layer, bevel info)

'sofi' = solid fill ( Photoshop 7.0) (see See Effects layer, solid fill (added in Photoshop 7.0))

Variable

See appropriate tables.

 
Effects layer, common state info
Length

Description

4

Size of next three items: 7

4

Version: 0

1

Visible: always true

2

Unused: always 0

 
Effects layer, drop shadow and inner shadow info
Length

Description

4

Size of the remaining items: 41 or 51 (depending on version)

4

Version: 0 ( Photoshop 5.0) or 2 ( Photoshop 5.5)

4

Blur value in pixels

4

Intensity as a percent

4

Angle in degrees

4

Distance in pixels

10

Color: 2 bytes for space followed by 4 * 2 byte color component

8

Blend mode: 4 bytes for signature and 4 bytes for key

1

Effect enabled

1

Use this angle in all of the layer effects

1

Opacity as a percent

10

Native color: 2 bytes for space followed by 4 * 2 byte color component

 
Effects layer, outer glow info
Length

Description

4

Size of the remaining items: 32 for Photoshop 5.0; 42 for 5.5

4

Version: 0 for Photoshop 5.0; 2 for 5.5

4

Blur value in pixels.

4

Intensity as a percent

10

Color: 2 bytes for space followed by 4 * 2 byte color component

8

Blend mode: 4 bytes for signature and 4 bytes for the key

1

Effect enabled

1

Opacity as a percent

10

(Version 2 only) Native color space. 2 bytes for space followed by 4 * 2 byte color component

 
Effects layer, inner glow info
Length

Description

4

Size of the remaining items: 33 for Photoshop 5.0; 43 for 5.5

4

Version: 0 for Photoshop 5.0; 2 for 5.5.

4

Blur value in pixels.

4

Intensity as a percent

10

Color: 2 bytes for space followed by 4 * 2 byte color component

8

Blend mode: 4 bytes for signature and 4 bytes for the key

1

Effect enabled

1

Opacity as a percent

Remaining fields present only in version 2

1

Invert

10

(Version 2 only) Native color space. 2 bytes for space followed by 4 * 2 byte color component

 
Effects layer, bevel info
Length

Description

4

Size of the remaining items (58 for version 0, 78 for version 20

4

Version: 0 for Photoshop 5.0; 2 for 5.5

4

Angle in degrees

4

Strength. Depth in pixels

4

Blur value in pixels.

8

Highlight blend mode: 4 bytes for signature and 4 bytes for the key

8

Shadow blend mode: 4 bytes for signature and 4 bytes for the key

10

Highlight color: 2 bytes for space followed by 4 * 2 byte color component

10

Shadow color: 2 bytes for space followed by 4 * 2 byte color component

1

Bevel style

1

Hightlight opacity as a percent

1

Shadow opacity as a percent

1

Effect enabled

1

Use this angle in all of the layer effects

1

Up or down

The following are present in version 2 only

10

Real highlight color: 2 bytes for space; 4 * 2 byte color component

10

Real shadow color: 2 bytes for space; 4 * 2 byte color component

 
Effects layer, solid fill (added in Photoshop 7.0)
Length

Description

4

Size: 34

4

Version: 2

4

Key for blend mode

10

Color space

1

Opacity

1

Enabled

10

Native color space

Type Tool Info (Photoshop 5.0 and 5.5 only)

Has been superseded in Photoshop 6.0 and beyond by a different structure with the key 'TySh' (see See Type tool object setting (Photoshop 6.0) See Type tool object setting ).

Key is 'tySh' . Data is as follows:

Type tool Info
Length

Description

2

Version ( = 1)

48

6 * 8 double precision numbers for the transform information

Font information

2

Version ( = 6)

2

Count of faces

The next 8 fields are repeated for each count specified above

2

Mark value

4

Font type data

Variable

Pascal string of font name

Variable

Pascal string of font family name

Variable

Pascal string of font style name

2

Script value

4

Number of design axes vector to follow

4

Design vector value

Style information

2

Count of styles

The next 10 fields are repeated for each count specified above

2

Mark value

2

Face mark value

4

Size value

4

Tracking value

4

Kerning value

4

Leading value

4

Base shift value

1

Auto kern on/off

1

Only present in version <= 5

1

Rotate up/down

Text information

2

Type value

4

Scaling factor value

4

Sharacter count value

4

Horizontal placement

4

Vertical placement

4

Select start value

4

Select end value

2

Line count, i.e. the number of items to follow.

The next 5 fields are repeated for each item in line count.

4

Character count value

2

Orientation value

2

Alignment value

2

Actual character as a double byte character

2

Style value

Color information

2

Color space value

8

4 * 2 byte color component

1

Anti alias on/off

Unicode layer name (Photoshop 5.0)

Key is 'luni' . Data is as follows:

Unicode Layer name
Length

Description

Variable

Unicode string

Layer ID (Photoshop 5.0)

Key is 'lyid' .

Layer ID
Length

Description

4

Signature: '8BIM'

4

Key: 'lyid'

4

Length: 4

4

ID.

Object-based effects layer info (Photoshop 6.0)

Key is 'lfx2' . Data is as follows:

Object Based Effects Layer info
Length

Description

4

Object effects version: 0

4

Descriptor version ( = 16 for Photoshop 6.0).

Variable

Descriptor (see See Descriptor structure)

Patterns (Photoshop 6.0 and CS (8.0))

This is a list of patterns. Key is 'Patt', 'Pat2' or 'Pat3' . Data is as follows:

Patterns
Length

Description

The following is repeated for each pattern.

4

Length of this pattern

4

Version ( =1)

4

The image mode of the file. Supported values are: Bitmap = 0; Grayscale = 1; Indexed = 2; RGB = 3; CMYK = 4; Multichannel = 7; Duotone = 8; Lab = 9.

4

Point: vertical, 2 bytes and horizontal, 2 bytes

Variable

Name: Unicode string

Variable

Unique ID for this pattern: Pascal string

Variable

Index color table (256 * 3 RGB values): only present when image mode is indexed color

Variable

Pattern data as Virtual Memory Array List


Virtual Memory Array List
Length

Description

4

Version ( =3)

4

Length

16

Rectangle: top, left, bottom, right

4

Number of channels

The following is a virtual memory array, repeated for the number of channels + one for a user mask + one for a sheet mask.

4

Boolean indicating whether array is written, skip following data if 0.

4

Length, skip following data if 0.

4

Pixel depth: 1, 8, 16 or 32

16

Rectangle: top, left, bottom, right

2

Pixel depth: 1, 8, 16 or 32

1

Compression mode of data to follow. 1 is zip.

Variable

Actual data based on parameters and compression

Annotations (Photoshop 6.0)

Key is 'Anno' . Data is as follows:

Annotations
Length

Description

2

Major version ( = 2)

2

Minor version. ( = 1)

4

Count of annotations to follow

Following is repeated for each annotation

4

Length of this annotation

4

Annotation type: either text( 'txtA' ) or sound ( 'sndA' ).

1

Is the annotation open

1

Flags.

2

Optional blocks. ( =1 for Photoshop 6.0)

16

Rectangle of icon location: top, left, bottom and right.

16

Rectangle of popup locations: top, left, bottom and right

10

2 bytes for space followed by 4 * 2 byte color component

Variable

Pascal string of author's name aligned to 2 bytes

Variable

Pascal string of name aligned to 2 bytes

Variable

Pascal string of the mod Date aligned to 2 bytes

4

Length of the following 3 fields including this field

4

' txtC ' or ' sndM '. Either text or sound

4

Length of the next field

Variable

Actual data for this annotation. The text is an ASCII or Unicode string; the sound annotation is documented in the PDF Reference , available at http://Partners.adobe.com/asn/developer/acrosdk/docs.html#filefmtspecs

Variable

Padding to align to multiple of 4 bytes


Blend clipping elements (Photoshop 6.0)

Key is 'clbl' . Data is as follows:

Blend clipping elements
Length

Description

1

Blend clipped elements: boolean

3

Padding


Blend interior elements (Photoshop 6.0)

Key is'infx' . Data is as follows:

Blend interior elements
Length

Description

1

Blend interior elements: boolean

3

Padding

 

Knockout setting (Photoshop 6.0)

Key is 'knko' . Data is as follows:

Knockout setting
Length

Description

1

Knockout: boolean

3

Padding

 

Protected setting (Photoshop 6.0)

Key is 'lspf' . Data is as follows:

Protected setting
Length

Description

4

Protection flags: bits 0 - 2 are used for Photoshop 6.0. Transparency, composite and position respectively.

Sheet color setting (Photoshop 6.0)

Key is 'lclr' . Data is as follows:

Sheet Color setting
Length

Description

4 * 2

Color. Only the first color setting is used for Photoshop 6.0; the rest are zeros

 

Reference point (Photoshop 6.0)

Key is 'fxrp' . Data is as follows:

Reference point
Length

Description

2 * 8

2 double values for the reference point

 

Gradient settings (Photoshop 6.0)

Key is 'grdm' . Data is as follows:

Gradient settings
Length

Description

2

Version ( =1 for Photoshop 6.0)

1

Is gradient reversed

1

Is gradient dithered

Variable

Name of the gradient: Unicode string, padded

2

Number of color stops to follow

Following is repeated for each color stop

4

Location of color stop

4

Midpoint of color stop

2

Mode for the color to follow

4 * 2

Actual color for the stop

2

Number of transparency stops to follow

Following is repeated for each transparency stop

4

Location of transparency stop

4

Midpoint of transparency stop

2

Opacity of transparency stop

2

Expansion count ( = 2 for Photoshop 6.0)

2

Interpolation if length above is non-zero

2

Length (= 32 for Photoshop 6.0)

2

Mode for this gradient

4

Random number seed

2

Flag for showing transparency

2

Flag for using vector color

4

Roughness factor

2

Color model

4 * 2

Minimum color values

4 * 2

Maximum color values

2

Dummy: not used in Photoshop 6.0

 

Section divider setting (Photoshop 6.0)

Key is 'lsct' . Data is as follows:

Section Divider setting
Length

Description

4

Type. 4 possible values, 0 = any other type of layer, 1 = open "folder", 2 = closed "folder", 3 = bounding section divider, hidden in the UI

Following is only present if length >= 12

4

Signature: '8BIM'

4

Key. See blend mode keys in See Layer records.

Following is only present if length >= 16

4

Sub type. 0 = normal, 1 = scene group, affects the animation timeline.

 
Channel blending restrictions setting (Photoshop 6.0)

Key is 'brst' . Data is as follows:

Channel blending restrictions setting
Length

Description

Following is repeated length / 4 times.

4

Channel number that is restricted

 
Solid color sheet setting (Photoshop 6.0)

Key is 'SoCo' . Data is as follows:

Solid color sheet setting
Length

Description

4

Version ( = 16 for Photoshop 6.0)

Variable

Descriptor. Based on the Action file format structure (see See Descriptor structure)

 
Pattern fill setting (Photoshop 6.0)

Key is 'PtFl' . Data is as follows:

Pattern fill setting
Length

Description

4

Version ( =16 for Photoshop 6.0)

Variable

Descriptor. Based on the Action file format structure (see See Descriptor structure)

 
Gradient fill setting (Photoshop 6.0)

Key is 'GdFl' . Data is as follows:

Gradient Fill Setting
Length

Description

4 bytes

Version ( = 16 for Photoshop 6.0)

Variable

Descriptor. Based on the Action file format structure (see See Descriptor structure)

 
Vector mask setting (Photoshop 6.0)

Key is 'vmsk' or 'vsms'. If key is 'vsms' then we are writing for (Photoshop CS6) and the document will have a 'vscg' key. Data is as follows:

Vector mask setting
Length

Description

4

Version ( = 3 for Photoshop 6.0)

4

Flags. bit 1 = invert, bit 2 = not link, bit 3 = disable

The rest of the data is path components, loop until end of the length.

Variable

Paths. See See Path resource format

 
Type tool object setting (Photoshop 6.0)

This supersedes the type tool info in Photoshop 5.0 (see See Type tool Info).

Key is 'TySh' . Data is as follows:

Type tool object setting
Length

Description

2

Version ( =1 for Photoshop 6.0)

6 * 8

Transform: xx, xy, yx, yy, tx, and ty respectively.

2

Text version ( = 50 for Photoshop 6.0)

4

Descriptor version ( = 16 for Photoshop 6.0)

Variable

Text data (see See Descriptor structure)

2

Warp version ( = 1 for Photoshop 6.0)

4

Descriptor version ( = 16 for Photoshop 6.0)

Variable

Warp data (see See Descriptor structure)

4 * 8

left, top, right, bottom respectively.

 
Foreign effect ID (Photoshop 6.0)

Key is 'ffxi' . Data is as follows:

Foreign effect ID
Length

Description

4

ID of the Foreign effect.

 
Layer name source setting (Photoshop 6.0)

Key is 'lnsr' . Data is as follows:

Layer name source setting
Length

Description

4

ID for the layer name

 
Pattern data (Photoshop 6.0)

Key is 'shpa' . Data is as follows:

Pattern data
Length

Description

4

Version ( = 0 for Photoshop 6.0)

4

Count of sets to follow

The following is repeated for the count above.

4

Pattern signature

4

Pattern key

4

Count of patterns in this set

1

Copy on sheet duplication

3

Padding

The following is repeated for the count of patterns above.

4

Color handling. Prefer convert = 'conv' , avoid conversion = 'avod' , luminance only = 'lumi'

Variable

Pascal string name of the pattern

Variable

Unicode string name of the pattern

Variable

Pascal string of the unique identifier for the pattern

 
Metadata setting (Photoshop 6.0)

Key is 'shmd' . Data is as follows:

Metadata setting
Length

Description

4

Count of metadata items to follow

The following is repeated the number of times specified by the count above:

4

Signature of the data

4

Key of the data

1

Copy on sheet duplication

3

Padding

4

Length of data to follow

Variable

Undocumented data

 
Layer version (Photoshop 7.0)

Key is 'lyvr' . Data is as follows:

Layer version
Length

Description

4

A 32-bit number representing the version of Photoshop needed to read and interpret the layer without data loss. 70 = 7.0, 80 = 8.0, etc.

The minimum value is 70, because just having the field present in 6.0 triggers a warning. For the future, Photoshop 7 checks to see whether this number is larger than the current version -- i.e., 70 -- and if so, warns that it is ignoring some data.

 
Transparency shapes layer (Photoshop 7.0)

Key is 'tsly' . Data is as follows:

Transparency shapes layer
Length

Description

1

1: the transparency of the layer is used in determining the shape of the effects. This is the default for behavior like previous versions.

0: treated in the same way as fill opacity including modulating blend modes, rather than acting as strict transparency.

Using this feature is useful for achieving effects that otherwise would require complex use of clipping groups.

3

Padding

 
Layer mask as global mask (Photoshop 7.0)

Key is 'lmgm' . Data is as follows:

Layer mask as global mask
Length

Description

1

1: the layer mask is used in a final crossfade masking the layer and effects rather than being used to shape the layer and its effects.

This behavior was previously tied to the link status flag for the layer mask. (An unlinked mask acted like a flag value of 1, a linked mask like 0). For old files that lack this key, the link status is used in order to preserve compositing results.

3

Padding

 
Vector mask as global mask (Photoshop 7.0)

Key is 'vmgm' . Data is as follows:

Vector mask as global mask
Length

Description

1

Same as in See Layer mask as global mask, but applying the vector mask.

3

Padding

 
Brightness and Contrast

Key is 'brit' . Data is as follows:

Brightness and Contrast
Length

Description

2

Brightness

2

Contrast

2

Mean value for brightness and contrast

1

Lab color only

 
Channel Mixer

Key is 'mixr' . Data is as follows:

Channel Mixer
Length

Description

2

Version ( = 1)

2

Monochrome

20

RGB or CMYK color plus constant for the mixer settings. 4 * 2 bytes of color with 2 bytes of constant.

 
Color Lookup (Photoshop CS6)

Key is 'clrL' . Data is as follows:

Color Lookup
Length

Description

2

Version ( = 1)

4

Descriptor Version ( = 16)

Variable

Descriptor of black and white information

 
Placed Layer (replaced by SoLd in Photoshop CS3)

Key is 'plLd' . Data is as follows:

Placed Layer
Length

Description

4

Type ( = 'plcL' )

4

Version ( = 3 )

Variable

Unique ID as a pascal string

4

Page number

4

Total pages

4

Anit alias policy

4

Placed layer type: 0 = unknown, 1 = vector, 2 = raster, 3 = image stack

4 * 8

Transformation: 8 doubles for x,y location of transform points

4

Warp version ( = 0 )

4

Warp descriptor version ( = 16 )

Variable

Descriptor for warping information

 
Linked Layer

Key is 'lnkD' . Also keys 'lnk2' and 'lnk3' . Data is as follows:

Linked Layer
Length

Description

The following is repeated for each linked file.

8

Length of the data to follow

4

Type ( = 'liFD' linked file data, 'liFE' linked file external or 'liFA' linked file alias )

4

Version ( = 1 to 7 )

Variable

Pascal string. Unique ID.

Variable

Unicode string of the original file name

4

File Type

4

File Creator

8

Length of the data to follow

1

File open descriptor

Variable

Descriptor of open parameters. Only present when above is true.

If the type is 'liFE' then a linked file Descriptor is next.

Variable

Descriptor of linked file parameters. See comment above.

If the type is 'liFE' and the version is greater than 3 then the following is present. Year, Month, Day, Hour, Minute, Second is next.

4

Year

1

Month

1

Day

1

Hour

1

Minute

8

Double for the seconds

If the type is 'liFE' then a file size is next.

8

File size

If the type is 'liFA' then 4 zeros are next.

8

All zeros

If the type is 'liFE' then they bytes of the file are next.

Variable

Raw bytes of the file.

If the version is greater than or equal to 5 then the following is next.

UnicodeString

Child Document ID.

If the version is greater than or equal to 6 then the following is next.

Double

Asset mod time.

If the version is greater than or equal to 7 then the following is next.

1

Asset locked state, for Libraries assets.

If the type is 'liFE' and the version is 2 then the following is next.

Variable

Raw bytes of the file.

 
Photo Filter

Key is 'phfl' . Data is as follows:

Photo Filter
Length

Description

2

Version ( = 3) or ( = 2 )

12

4 bytes each for XYZ color (Only in Version 3)

10

2 bytes color space followed by 4 * 2 bytes color component (Only in Version 2)

4

Density

1

Preserve Luminosity

 
Black White (Photoshop CS3)

Key is 'blwh' . Data is as follows:

Black White
Length

Description

4

Descriptor Version ( = 16)

Variable

Descriptor of black and white information

 
Content Generator Extra Data (Photoshop CS5)

Key is 'CgEd' . Data is as follows:

Content Generator Extra Data
Length

Description

4

Descriptor Version ( = 16)

Variable

Descriptor of extra data

 
Text Engine Data (Photoshop CS3)

Key is 'Txt2' . Data is as follows:

Text Engine Data
Length

Description

4

Length of data to follow

Variable

Raw bytes for text engine

 
Vibrance (Photoshop CS3)

Key is 'vibA' . Data is as follows:

Vibrance
Length

Description

4

Descriptor Version ( = 16)

Variable

Descriptor of vibrance information

 
Unicode Path Name (Photoshop CS6)

Key is 'pths' . Data is as follows:

Unicode Path Name
Length

Description

4

Descriptor Version ( = 16)

Variable

Descriptor containing a list of unicode path names

 
Animation Effects (Photoshop CS6)

Key is 'anFX' . Data is as follows:

Animation Effects
Length

Description

4

Descriptor Version ( = 16)

Variable

Descriptor containing animation effects

 
Filter Mask (Photoshop CS3)

Key is 'FMsk' . Data is as follows:

Filter Mask
Length

Description

10

Color space

2

Opacity

 
Placed Layer Data (Photoshop CS3)

Key is 'SoLd' . See also 'PlLd' key. Data is as follows:

Filter Mask
Length

Description

4

Identifier ( = 'soLD' )

4

Version ( = 4 )

4

Descriptor Version ( = 16)

Variable

Descriptor of placed layer information

 
Vector Stroke Data (Photoshop CS6)

Key is 'vstk' . Data is as follows:

Vector stroke setting
Length

Description

4

Version ( = 16 )

Variable

Descriptor. Based on the Action file format structure (see See Descriptor structure)

 
Vector Stroke Content Data (Photoshop CS6)

Key is 'vscg' . Data is as follows:

Vector stroke content setting
Length

Description

4

Key for data

4

Version ( = 16 )

Variable

Descriptor. Based on the Action file format structure (see See Descriptor structure)

 
Using Aligned Rendering (Photoshop CS6)

Key is 'sn2P' . Data is as follows:

Using Aligned Rendering
Length

Description

4

Non zero is true for using aligned rendering

 
Vector Origination Data (Photoshop CC)

Key is 'vogk' . Data is as follows:

Vector origination setting
Length

Description

4

Version ( = 1 for Photoshop CC)

4

Version ( = 16 )

Variable

Descriptor. Based on the Action file format structure (see See Descriptor structure)

 
Pixel Source Data (Photoshop CC)

Key is 'PxSc'. Data is as follows:

Pixel Source info
Length

Description

4

Version ( = 16 )

Variable

Descriptor. Based on the Action file format structure (see See Descriptor structure)

 
Compositor Used (Photoshop 2020)

Key is 'cinf'. Data is as follows:

Compositor Used
Length

Description

4

Version ( = 16 )

Variable

Descriptor. Based on the Action file format structure (see See Descriptor structure)

 
Pixel Source Data (Photoshop CC 2015)

Key is 'PxSD'. Data is as follows:

Pixel Source info
Length

Description

8

Length of data to follow

Variable

Raw data for 3D or video layers.

 
Artboard Data (Photoshop CC 2015)

Key is 'artb' or 'artd' or 'abdd'. Data is as follows:

Artboard info
Length

Description

4

Version ( = 16 )

Variable

Descriptor. Based on the Action file format structure (see See Descriptor structure)

 
Smart Object Layer Data (Photoshop CC 2015)

Key is 'SoLE' . Data is as follows:

Smart Object info
Length

Description

4

Type ( = 'soLD' )

4

Version ( = 4 or 5 )

Variable

Descriptor. Based on the Action file format structure (see See Descriptor structure)

 
Saving Merged Transparency

Key is 'Mtrn', 'Mt16' or 'Mt32' . There is no data associated with these keys.

 

User Mask

Key is 'LMsk' .

User Mask
Length

Description

10

Color space

2

Opacity

1

Flag ( = 128 )

 

Exposure

Key is 'expA' .

Exposure
Length

Description

2

Version (= 1)

4

Exposure

4

Offset

4

Gamma

 

Filter Effects

Key is 'FXid' or 'FEid' .

Filter Effects
Length

Description

4

Version ( =1, 2 or 3)

8

Length of data to follow

The following is repeated for the given length.

Variable

Pascal string as identifier

4

Version ( = 1 )

8

Length

16

Rectangle: top, left, bottom, right

4

Depth

4

Max channels

The following is repeated for number of channels + a user mask + a sheet mask.

4

Boolean indicating whether array is written

8

Length

2

Compression mode of data to follow.

Variable

Actual data based on compression

End of repeating for channels

1

Next two items present or not

2

Compression mode of data to follow

Variable

Actual data based on compression

Image Data Section
The last section of a Photoshop file contains the image pixel data. Image data is stored in planar order: first all the red data, then all the green data, etc. Each plane is stored in scan-line order, with no pad bytes,

Image data section
Length

Description

2

Compression method:

0 = Raw image data

1 = RLE compressed the image data starts with the byte counts for all the scan lines (rows * channels), with each count stored as a two-byte value. The RLE compressed data follows, with each scan line compressed separately. The RLE compression is the same compression algorithm used by the Macintosh ROM routine PackBits , and the TIFF standard.

2 = ZIP without prediction

3 = ZIP with prediction.

Variable

The image data. Planar order = RRR GGG BBB, etc.

 
Other Document File Formats
Photoshop EPS files
The following summarizes the additional information Photoshop writes when creating EPS files:

Photoshop writes a high-resolution bounding box comment to the EPS file immediately following the traditional EPS bounding box comment. The comment begins with " %%HiResBoundingBox " and is followed by four numbers identical to those given for the bounding box except that they can have fractional components (i.e., a decimal point and digits after it). The traditional bounding box is written as the rounded version of the high resolution bounding box for compatibility.

Photoshop writes its image resources out to a block of data stored as follows:

%BeginPhotoshop: <length> <hex data>

EPS parameters for BeginPhotoshop
Field

Definition

length

Length of the image resource data.

hex data

Image resource data in hexadecimal.

Photoshop includes a comment in the EPS files it writes so that it is able to read them back in again. Third party programs that write pixel-based EPS files may want to include this comment in their EPS files, so Photoshop can read their files.

The comment must follow immediately after the %% comment block at the start of the file. The comment is:

%ImageData: <columns> <rows> <depth> <mode> <pad channels> <block size> <binary/hex> "<data start>"

 

EPS parameters for ImageData
Field

Definition

columns

Width of the image in pixels.

rows

Height of the image in pixels.

depth

Number of bits per channel. Must be 1 or 8.

mode

Image mode. Bitmap/grayscale = 1; Lab = 2; RGB = 3; CMYK = 4.

pad channels

Number of other channels store in the file. Ignored when reading. Photoshop uses this to include a grayscale image that is printed on non-color PostScript printers.

block size

Number of bytes per row per channel. Will be either 1 or formula (below):

1 = Data is interleaved.

(columns*depth+7)/8 =Data is stored in line-interleaved format, or there is only one channel.

binary/ascii

1 = Data is in binary format.

2 = Data is in hex ascii format.

data start

Entire PostScript line immediately preceding the image data. This entire line should not occur elsewhere in the PostScript header code, butit may occur at part of a line.

TIFF files
See TIFF Tags describes the standard TIFF (version 6) tags and tag values that Photoshop is able to read and write. Photoshop reads the first Image File Directory (IFD) and writes one IFD per file.

In addition, Photoshop uses a set of tags that are not defined in the TIFF v6 specification to store specific information. See See Photoshop-specific TIFF Tags.

See See TIFF Files on Mac OS for information about how TIFF files are stored on Macintosh.

TIFF Tags
Tag

Name

Photoshop reads

Photoshop writes

254

NewSubFileType

Ignored

0

256

ImageWidth

1 to 30000

1 to 30000

257

ImageLength

1 to 30000

1 to 30000

258

BitsPerSample

1, 2, 4, 8, 16 (all same)

1, 8, 16

259

Compression

1 (uncompressed), 2 (CCITT), 5 (LZW), 7 (JPEG), 8 (ZIP), 32773 (PackBits)

1, 5, 7, 8

262

PhotometricInterpretation

0, 1, 2, 3, 5, 8, 9

0 (1-bit), 1 (8-bit), 2, 3,5,8

266

FillOrder

1

No

270

ImageDescription

Printing Caption

Printing Caption

271

EXIF: Make

 

 

272

EXIF: Model

 

 

273

StripOffsets

Yes

Yes

277

SamplesPerPixel

1 to 24

1 to 24

278

RowsPerStrip

Any

Single strip if not compressed, multiple strips if compressed.

279

StripByteCounts

Required if compressed

Yes

282

XResolution

Yes

Yes

283

YResolution

Ignored (square pixels assumed)

Yes

284

PlanarConfiguration

1 or 2

1

296

ResolutionUnit

2 or 3

2

305

EXIF: Software

 

 

306

EXIF: Date/time

 

 

315

EXIF: Artist

 

 

317

Predictor

1 or 2

1 or 2

320

ColorMap

Yes

Yes

322

TileWidth

Yes

No

323

TileLength

Yes

No

324

TileOffsets

Yes

No

325

TileByteCounts

Required if compressed

No

332

InkSet

1

No

336

DotRange

Yes, if CMYK

Yes

338

ExtraSamples

Ignored (except for count)

Photoshop 5.5 and earlier writes 0. Photoshop 6.0 and later writes 0 or 1 based on the spec.

See Photoshop TIFF.pdf for additional information about tags 259 and 262.

Photoshop-specific TIFF Tags
Photoshop-specific TIFF tags
Tag

Description

330

tSubIFD . Documented in the TIFF-PM6.pdf file as a PageMaker extension

437

JPEG tables. See Photoshop TIFF.pdf for more information.

700

XMP metadata. See http://www.adobe.com/devnet/xmp/

33723

File information (IPTC-NAA record 2: see the documents in the IPTC folder of the Documentation folder).

34377

Photoshop image resources (see See Image Resources Section)

34665

EXIF IFD pointer. See http://www.kodak.com/global/plugins/acrobat/en/service/digCam/exifStandard2.pdf

34675

ICC Profiles (see the ICC1v42_2006-05.pdf file from the International Color Consortium in the Documentation folder of the Photoshop SDK)

34853

EXIF GPS info. See http://www.kodak.com/global/plugins/acrobat/en/service/digCam/exifStandard2.pdf

37724

tImageSourceData . Begins with the null-terminated string " Adobe Photoshop Document Data Block ", (**PSB** " Adobe Photoshop Document Data V0002 "), followed by data of various types. See Photoshop TIFF.pdf for a list .

50255

tAnnotations . See See Annotations for details.

TIFF Files on Mac OS
For cross-platform compatibility, all information in a Macintosh TIFF file is stored in the data fork. For interoperability with other Mac OS applications, however, some information is duplicated in resources stored in the resource fork of the file.

For compatibility with image cataloging applications, the 'pnot' resource id 0 contains references to thumbnail, keywords, and caption information stored in other resources.

The thumbnail picture is stored in a 'PICT' resource, the keywords are stored in 'STR#' resource 128 and the caption text is stored in 'TEXT' resource 128. For more information on the format of these resources see Inside Macintosh: QuickTime Components and the Extensis Fetch Awareness Developer's Toolkit .

All of the data from Photoshop's File Info dialog is stored in 'ANPA' resource 10000.

'STR ' resource -16396 contains a string indicating the application that created the TIFF file.

Photoshop also creates 'icl8' -16455 and 'ICN#' -16455 resources containing thumbnail images which are shown in the Mac OS Finder.

 
Additional File Formats
In addition to documents that the user creates in Adobe Photoshop (discussed in See The Photoshop File Format), there are a number of additional files used by Photoshop to store information about such items as colors, contours, curves, levels and so forth. These are known as load files.

This chapter describes the format of each load file. Some of the files can saved by the user; others are load only, as indicated in the sections.

Each file has a unique file type and file extension associated with it. Photoshop for Macintosh recognizes either, but does not require the use of the extension. In the file dialogs, Photoshop for Windows looks for files with the given file extension automatically; this can be overridden.

Under Mac OS, all information is stored in the data forks of Photoshop's load files. The files are completely interchangable with Windows or any other platform.

Consistent byte ordering is required across platforms when reading and writing load files. Photoshop stores multi-byte values with the high-order bytes first, (big-endian), as on Mac OS., which is the opposite of Windows' standard byte order.. For more information, see "Macintosh and Windows development" in chapter 2 of Photoshop API Guide.pdf .

Actions
Actions are accessed by means of the Actions palette. The object effects use the actions mechanism to output information to the PSD file format.

Action file types
OS

Filetype/extension

Mac OS

8BAC

Windows

.ATN

Each action file comprises an action set . The format of the action file is described in the table below:

Action file format
Length

Description

4

Version ( = 16)

Variable

Unicode string: action set name

1

Boolean: true if set is expanded for the Actions palette

4

Number of actions in action set

The following is repeated for each action in the set

2

Index of action

1

Boolean: true if Shift key needed for keyboard shortcut

1

Boolean: true if Command key needed for keyboard shortcut

2

Color index information

Variable

Unicode string: action name

1

Boolean: true if action is expanded in the Actions palette

4

Number of items in action

The following is repeated for each item

1

Boolean: true if action is expanded in the Actions palette

1

Boolean: true if action is enabled

1

Boolean: true if dialogs should be displayed

1

Options for displaying dialogs

4

Identifier: 'TEXT' or 'long'

Variable

Event: if identifier is 'TEXT' ,4 bytes of length followed by the string;

if identifier is 'long' , 4 bytes of itemID

Variable

Dictionary name: 4 bytes of length followed by the string

4

-1 if a descriptor follows or 0 for none.

Variable

Descriptor: see Descriptor structure (See Descriptor structure) for details

Descriptor structure
Length

Description

Variable

Unicode string: name from classID

Variable

classID: 4 bytes (length), followed either by string or (if length is zero) 4-byte classID

4

Number of items in descriptor

The following is repeated for each item in descriptor

Variable

Key: 4 bytes ( length) followed either by string or (if length is zero) 4-byte key

4

Type: OSType key

'obj ' = Reference

'Objc' = Descriptor

'VlLs' = List

'doub' = Double

'UntF' = Unit float

'TEXT' = String

'enum' = Enumerated

'long' = Integer

'comp' = Large Integer

'bool' = Boolean

'GlbO' = GlobalObject same as Descriptor

'type' = Class

'GlbC' = Class

'alis' = Alias

'tdta' = Raw Data

Variable

Item type: see the tables below for each possible type

Reference Structure
Length

Description

4

Number of items

The following is repeated for each item in reference

4

OSType key for type to use:

'prop' = Property

'Clss' = Class

'Enmr' = Enumerated Reference

'rele' = Offset

'Idnt' = Identifier

'indx' = Index

'name' =Name

Variable

Item type: see the tables below for each possible Reference type

Property Structure
Length

Description

Variable

Unicode string: name from classID

Variable

classID: 4 bytes (length), followed either by string or (if length is zero) 4-byte classID

Variable

KeyID: 4 bytes (length), followed either by string or (if length is zero) 4-byte keyID

'#Pnt' = points: tagged unit value

'#Mlm' = millimeters: tagged unit value

Unit float structure
Length

Description

4

Units the following value is in. One of the following:


'#Ang' = angle: base degrees

'#Rsl' = density: base per inch

'#Rlt' = distance: base 72ppi

'#Nne' = none: coerced.

'#Prc'= percent: unit value

'#Pxl' = pixels: tagged unit value

8

Actual value (double)

Double structure
Length

Description

8

Actual value (double)

Class structure
Length

Description

Variable

Unicode string: name from classID

Variable

ClassID: 4 bytes (length), followed either by string or (if length is zero) 4-byte classID

String structure
Length

Description

Variable

String value as Unicode string

Enumerated reference
Length

Description

Variable

Unicode string: name from ClassID.

Variable

ClassID: 4 bytes (length), followed either by string or (if length is zero) 4-byte classID

Variable

TypeID: 4 bytes (length), followed either by string or (if length is zero) 4-byte typeID

Variable

enum: 4 bytes (length), followed either by string or (if length is zero) 4-byte enum

Offset structure
Length

Description

Variable

Unicode string: name from ClassID

Variable

ClassID: 4 bytes (length), followed either by string or (if length is zero) 4-byte classID

4

Value of the offset

Boolean structure
Length

Description

1

Boolean value

Alias structure
Length

Description

4

Length of data to follow

Variable

FSSpec for Macintosh or a handle to a string to the full path on Windows

List structure
Length

Description

4

Number of items in the list

The following is repeated for each item in list

4

OSType key for type to use. See See Descriptor structure for types.

Variable

See the tables above for each possible type

 
Large Integer
Length

Description

8

Value

 
Integer
Length

Description

4

Value

 
Enumerated descriptor
Length

Description

Variable

Type: 4 bytes (length), followed either by string or (if length is zero) 4-byte typeID

Variable

Enum: 4 bytes (length), followed either by string or (if length is zero) 4-byte enum

 
Raw Data
Length

Description

Variable

Value

Arbitrary Map
Arbitrary Map files are accessed by means of the Curves dialog ( load only ).

Arbitrary map file types
OS

Filetype/extension

Mac OS

8BLT

Windows

.AMP

There is no version number written in the file.

The files are an even multiple of 256 bytes long. Each 256 bytes is a lookup table, where:

The first byte of the table corresponds to byte zero of the image.

The last byte of the table corresponds to byte 255 of the image.

A NULL table that has no effect on an image is a linear table of bytes from 0 to 255.

If the file has one table, it is applied to the image's channels according to these priorities:

If the image has a master composite channel, the table is applied to it. If not, then:

If the image has a single active channel, the table is applied to it. If not, then:

If the image has no composite channel and more than one active channel, the table is not applied.

If the file has exactly three tables, it is applied to the image's channels according to these priorities:

The tables are assumed to represent RGB lookups. They are applied to the first three channels in the image, leaving the master composite untouched. Or:

If the image has a single active channel, the tables are converted to grayscale and the result is applied to the active channel. Or:

The first table is treated as a master. The remaining tables are applied to the image channels in turn (second table is applied to first channel, third table is applied to second channel, etc.).

Single active channels
Photoshop handles single active channels in a special fashion. When saving a map applied to a single channel, only one table is written to the file. Similarly, when reading a file for application to a single active channel, the master table is the one that will be used on that channel. This allows easy application of a single file to both composite and grayscale images.

CMYK Setup
CMYK settings files are accessed in Photoshop's Color Settings dialog (load only) .

CMYK file types
OS

Filetype/extension

Mac OS

8BIC

Windows

.API

CMYK setup file format
Length

Description

2

Version ( = 7)

27*2

Nine sets of three short integers specifying th\e xyY (CIE) values for the inks and their combinations. The inks are specified in the order cyan, magenta, yellow, magenta-yellow (red), cyan-yellow (green), cyan-magenta (blue), cyan-magenta-yellow, followed by the white and black points. Each set is written in the order xyY where:

x = 0...10000, representing 0.0...1.0000. y = 1...10000, representing 0.0001...1.0000. Y = 0...20000, representing 0.00...200.00.

2

Dot gain. Short integer from -10...40, representing -10%...40%.

1

Use curves. = 1 if curves table present.

1

Filler: zero

13*4*2

Only present if "use curves" = 1.

4 sets of 13 short integers specifyting the cyan, magenta, yellow, and black curve percentages from the Dot Gain Curves dialog. 0...1000, representing 0.0...100.0 %

Variable

Separation setup: see See Separation file format

 
Separation file format
Length

Description

2

Version ( = 300)

2

Separation type. 0 = UCR separations; 1 = GCR separations

2

Blank ink limit (0...100)

2

Total ink limit (200...400)

2

Undercolor addition for GCR separations (0...100)

Variable

Black generation (spline) curve detailed in See Black generation curve data structure. See also the Curves data format in See Curves file format.

Black generation curve data structure
Length

Description

2

Number of points in curve ( 2...19)

2* number of points

Each curve point is a pair of short integers where the first number is the output value (vertical coordinate on the Black Generation dialog graph) and the second is the input value. All coordinates have range 0 to 255. A NULL curve (no change to image data) is represented by the following five-number, ten-byte sequence in a file:

2 0 0 255 255.

The black generation curve and the UCA limit must both be present even if the separation type is set to UCR ( = 0).

Color Books
Color book files (Photoshop 7.0) are automatically loaded by Photoshop; they cannot be saved or loaded via a menu item. You can place custom color books into the Presets\Color Books folder. Use the Custom button on the Adobe color picker to access them.

Color book file types
OS

Filetype/extension

Mac OS

8BCB

Windows

.ACB

Color book file format
Length

Description

4

Signature: 8BCB

2

Version ( =1 )

2

Book ID. Existing IDs: 3000 (ANPA), 3001 (Focoltone), 3002 (PantoneCoated), 3003 (PantoneProcess), 3004 (PantoneProSlim), 3005 (PantoneUncoated), 3006 (Toyo), 3007 (Trumatch), 3008 (HKSE), 3009 (HKSK), 3010 (HKSN), 3011 (HKSZ), 3012 (DIC), 3020 (PantonePastelCoated), 3021 (PantonePastelUncoated), 3022 (PantoneMetallic)

Variable

Unicode string: title

Variable

Unicode string: prefix

Variable

Unicode string: postfix

Variable

Unicode string: description

2

Number of colors (<= 8000)

2

Colors per page (<= 9)

2

Key color page; must be less than or equal to colers per page

2

Color type. 0 = RGB; 2 = CMYK; 7 = Lab

The following are repeated for the number of colors

Variable

Unicode string: name

6

Unique key for the color

4

Color values: 4 bytes for CMYK; 3 bytes for RGB and Lab

Color Table
Color Table files are accessed using the Colors palette (load only) .

Color table file types
OS

Filetype/extension

Mac OS

8BCT

Windows

.ACT

There is no version number written in the file. The file is 768 or 772 bytes long and contains 256 RGB colors. The first color in the table is index zero. There are three bytes per color in the order red, green, blue. If the file is 772 bytes long there are 4 additional bytes remaining. Two bytes for the number of colors to use. Two bytes for the color index with the transparency color to use. If loaded into the Colors palette, the colors will be installed in the color swatch list as RGB colors.

Color Swatches
Color swatch files are loaded and saved in Photoshop's Color Swatches palette. These are typically stored in the Color Swatches sub-directory in the Presets directory.

Color swatches file types
OS

Filetype/extension

Mac OS

8BCO

Windows

.ACO

 
Color swatches file format
Length

Description

2

Version ( =1 )

2

Count of colors in the file.

count *10

Colors. Each color is 10 bytes, as described in See Color structure.

At the end of a version 1 file is the version 2 information.

2

Version ( = 2 )

2

Count of colors in the file. The next two fields are repeated for each count.

count *10

Colors. Each color is 10 bytes, as described in See Color structure.

Variable

Unicode string: color name.

Color structure
Length

Description

2

The color space the color belongs to (see See Color space IDs).

8

Four short unsigned integers with the actual color data. If the color does not require four values, the extra values are undefined and should be written as zeros. See See Color space IDs.

Color space IDs
Color ID

Description

0

RGB.

The first three values in the color data are red , green , and blue . They are full unsigned 16-bit values as in Apple's RGBColor data structure. Pure red = 65535, 0, 0.

1

HSB.

The first three values in the color data are hue , saturation , and brightness . They are full unsigned 16-bit values as in Apple's HSVColor data structure. Pure red = 0,65535, 65535.

2

CMYK.

The four values in the color data are cyan , magenta , yellow , and black . They are full unsigned 16-bit values.

0 = 100% ink. For example, pure cyan = 0,65535,65535,65535.

7

Lab.

The first three values in the color data are lightness , a chrominance , and b chrominance .

Lightness is a 16-bit value from 0...10000. Chrominance components are each 16-bit values from -12800...12700. Gray values are represented by chrominance components of 0. Pure white = 10000,0,0.

8

Grayscale.

The first value in the color data is the gray value, from 0...10000.

Photoshop allows the specification of custom colors, such as those colors that are defined in a set of custom inks provided by a printing ink manufacturer. These colors can be stored in the Colors palette and streamed to and from load files. The details of a custom color's color data fields are not public and should be treated as a black box.

See Custom color spaces gives the color space IDs currently defined by Photoshop for some custom color spaces.

Custom color spaces
Color ID

Name

3

Pantone matching system

4

Focoltone colour system

5

Trumatch color

6

Toyo 88 colorfinder 1050

10

HKS colors

Contours
Contour settings files (Photoshop 6.0) are loaded and saved in Photoshop's Layer Effects dialog.

Contour file types
OS

Filetype/extension

Mac OS

8BFS

Windows

.SHC

Contour file format
Length

Description

4

Type ( = '8BFS' )

2

Version ( = 1 )

4

Count of contours

The following is repeated for each contour

4

Version ( = 1 or 2)

Variable

Unicode string: contour name

Variable

version 1 or 2 data follows. See See Contours Version 1 for version 1 and See Contours Version 2 for version 2.

 
Contours Version 1
Length

Description

2

Count of points

4* count

For each point: 4 bytes of point data (2 bytes vertical, 2 bytes horizontal_

4

Minimum input range

4

Maximum input range

 
Contours Version 2
Length

Description

2

Count of points

4 * count

For each point: Point data (2 bytes vertical, 2 bytes horizontal)

1 * count

For each point: boolean indicating whether the point is continuous

4

Min input range

4

Max input range

Curves
Curves settings files are loaded in Photoshop's Curves dialog and Black Generation curve dialog (from within Separation Setup Preferences). Curves files can also be loaded into any of Photoshop's transfer function dialogs, such as the Duotone Curve dialog from within Duotone Options, and Print transfer dialog. Curves are saved as .ATF and .ACV files.

When loaded into a transfer function dialog, only the first curve in a Curves file is used.

Curves file types
OS

Filetype/extension

Mac OS

8BSC

Windows

.CRV

Curves file format
Length

Description

2

Version ( = 1 or = 4)

2

Version 1 = bit map of curves in file
Version 4 = count of curves in the file

The following is the data for each curve specified by count above

2

Count of points in the curve (short integer from 2...19)

point count * 4

Curve points. Each curve point is a pair of short integers where the first number is the output value (vertical coordinate on the Curves dialog graph) and the second is the input value. All coordinates have range 0 to 255. See also See Null curves below.

Null curves

A NULL curve (no change to image data) is represented by the following five-number, ten-byte sequence in a file:

2 0 0 255 255

Displaying ink percentages

Photoshop allows the option of displaying ink percentages instead of pixel values; this is a display option only and the internal data is unchanged, with 100% ink equal to image data of 0 and 0% ink equal to image data of 255.

Curves data order

The first curve is a master curve that applies to all the composite channels (RGB) when in composite image mode.

The remaining curves apply to the active channels in order: curve two applies to channel one, curve three applies to channel two, etc., up until curve 17, which applies to channel 16.

Indexed color

The exception to the normal order, and the reason there are up to 19 curves, is when the mode is Indexed color. In this case:

The first curve is a master curve.

The next three curves are created for the Red, Green, and Blue portions of the image's color table, and they are applied to the first channel.

The remaining curves apply to any remaining alpha channel that is active: for instance, if channel two is active, curve five applies to it; if channel three is active, curve six applies to it, etc., up until curve 19, which applies to channel 16.

Single active channels

Photoshop handles single active channels in a special fashion. When saving the curves applied to a single channel, the settings are stored into the master curve, at the beginning of the file. Similarly, when reading a curves file for application to a single active channel, the master curve is the one that will be used on that channel. This allows easy application of a single file to both RGB and grayscale images.

Additional information

At the end of the Version 1 file is the following information:

Extra level record info marker 'Crv '

Extra curves marker
Length

Description

4

= 'Crv ' for extra curve information

2

Version ( = 4)

4

Count of items to follow.

The following is the data for each curve specified by count above

2

Before each curve is a channel index.

2

Count of points in the curve (short integer from 2...19)

point count * 4

Curve points. Each curve point is a pair of short integers where the first number is the output value (vertical coordinate on the Curves dialog graph) and the second is the input value. All coordinates have range 0 to 255. See also See Null curves below.

Custom Kernel
Kernel settings files are loaded and saved in Photoshop's Custom Filter dialog. .

Custom kernel file types
OS

Filetype/extension

Mac OS

8BCK

Windows

.ACF


Custom filter structure
Length

Description

50

Weights.

The first 25 values are the custom weights from -999...999, applied to pixels offset from each pixel by [-2,-2] to [2,2]. The values progress through horizontal offsets first, as follows:

{[-2,-2],[-1,-2],[ 0,-2],[ 1,-2],[ 2,-2],

[-2,-1],[-1,-1],[ 0,-1],[ 1,-1],[ 2,-1],

[-2, 0],[-1, 0],[ 0, 0],[ 1, 0],[ 2, 0],

[-2, 1],[-1, 1],[ 0, 1],[ 1, 1],[ 2, 1],

[-2, 2],[-1, 2],[ 0, 2],[ 1, 2],[ 2, 2]}

27*2

Ink colors.

Nine sets of three short integers specifying the xyY (CIE) values for the inks and their combinations. The inks are specified in the order cyan, magenta, yellow, magenta-yellow (red), cyan-yellow (green), cyan-magenta (blue), cyan-magenta-yellow, followed by the white and black points. Each set is written in the order xyY where:

x = 0...10000, representing 0.0...1.0000. y = 1...10000, representing 0.0001...1.0000. Y = 0...20000, representing 0.00...200.00.

2

Scale. Short integer from 1...9999.

2

Offset. Short integer from -9999...9999.


Duotone Options
Duotone settings files are loaded and saved in the Duotone Options dialog..

Duotone file types
OS

Filetype/extension

Mac OS

8BDT

Windows

.ADO


Duotone file format
Length

Description

2

Version ( = 1)

2

Count . Number of plates in duotone spec (short integer). 1 = Monotone; 2 = Duotone; 3 = Tritone; 4 = Quadtone.

4*10

Four ink colors, regardless of the number of plates. The contents of the colors beyond the last plate specified by Count are undefined. Each color is 10 bytes and described in See Duotone color structure. It is identical to the format in a Colors load file.

4*64

Four ink names, regardless of the number of plates. Each name is streamed as a Pascal-style string with a length byte followed by the string name. Names may not be more than 63 characters. Each name is padded to occupy 64 bytes, including the length byte. Any names beyond the last plate specified by Count should be empty, size = 0.

4*28

Four ink curves, regardless of the number of plates. Described in See Ink curves structure.

2

Dot gain ( = 20). Kept for compatability with Photoshop 2.0. Ignored.

11*10

Eleven overprint colorscolors, regardless of the number of plates. The number of defined overprints depends on Count .

Monotones = no overprint colors. Duotones = one overprint color. Tritones = four overprint colors. Quadtones = 11 overprint colors. The contents of the colors beyond the last defined overprint are undefined. Each color is 10 bytes and described in See Duotone color structure. It is identical to the format in a Colors load file.


Duotone color structure
Length

Description

2

The color space the color belongs to (see See Color space IDs).

8

Four short unsigned integers with the actual color data. If the color does not require four values to specify, the extra values are undefined and should be written as zeros.


Ink curves structure
Length

Description

26

Transfer curve: Array of 13 short integers from 0...1000 representing 0.0...100.0. All but the first and last value may be -1, representing no point on the curve. Any curves beyond the last plate should be equal to the NULL curve. A NULL transfer curve looks like this: 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 1000.

2

Override ( = 0). Short integer for compatibility. Ignored by Photoshop 3.0 and higher.

Halftone Screens
Halftone Screens settings files are loaded and saved in Photoshop's Halftone Screens dialog (available from Edit > Print with Preview in Photoshop 7, or Page Setup or Print Options in previous versions).

Halftone screen file types
OS

Filetype/extension

Mac OS

8BHS

Windows

.AHS


Halftone screens file format
Length

Description

2

Version ( = 5)

4*18

Four screen descriptions. See See Halftone screen parameter structure.

Variable

For every screen that has a custom spot function, the PostScript function text is written here, one after the other, with no header information, in the same order as the screen settings. The size of each custom spot is the absolute value of its negative shape code.


Halftone screen parameter structure
Length

Description

4

Ink's screen frequency, in lines per inch. Binary fixed point value ;16 bits representing the integer and fractional parts from 1.0...999.999.

2

Units for the screen frequency. Lines per inch = 1; lines per centimeter = 2. Only affects display, not screen frequency.

4

Angle for screen. Binary fixed point value with 16 bits representing the integer and fractional parts from -180.0000 ... 180.0000, measured in degrees.

2

Code representing the shape of the halftone dots. 0 = Round; 1 = Ellipse; 2 = Line; 3 = Square; 4 = Cross; 6 = Diamond. Negative numbers represent custom shapes; the absolute value is the size in bytes of the custom spot function described in See Halftone screens file format.

4

= 0. Not currently used by Photoshop.

1

Boolean. 1 = Use accurate screens; 0 = Use other.

1

Boolean. 1 = Use printer's default screens; 0 = Use other.

Hue/Saturation
Hue/Saturation settings files are loaded and saved in Photoshop's Hue/Saturation dialog.

Hue/saturation file types
OS

Filetype/extension

Mac OS

8BHA

Windows

.AHU


Hue/saturation file format
Length

Description

2

Version ( = 2)

1

0 = Use settings for hue-adjustment; 1 = Use settings for colorization.

1

Padding byte; must be present but is ignored by Photoshop.

6

Colorization.

Photoshop 5.0: The actual values are stored for the new version. Hue is -180...180, Saturation is 0...100, and Lightness is -100...100.

Photoshop 4.0: Three short integers Hue, Saturation, and Lightness from -100...100. The user interface represents hue as -180...180, saturation as 0...100, and Lightness as -100...100, as the traditional HSB color wheel, with red = 0.

6

Master hue, saturation and lightness values.

6 sets of the following 14 bytes (4 range values followed by 3 settings values)

8: range values

For RGB and CMYK, those values apply to each of the six hextants in the HSB color wheel: those image pixels nearest to red, yellow, green, cyan, blue, or magenta. These numbers appear in the user interface from -60...60, however the slider will reflect each of the possible 201 values from -100...100.

For Lab, the first four of the six values are applied to image pixels in the four Lab color quadrants, yellow, green, blue, and magenta. The other two values are ignored ( = 0). The values appear in the user interface from -100 to 100.

6:settings values

Levels
Levels settings files are loaded and saved in the Levels dialog.

Levels file types
OS

Filetype/extension

Mac OS

8BLS

Windows

.ALV


Levels file format
Length

Description

2

Version ( = 2)

29 * 10

29 sets of level records, each level containing 5 short integers (see See Level record structure).


Level record structure
Length

Description

2

Input floor (0...253)

2

Input ceiling (2...255)

2

Output floor (0...255). Matched to input floor.

2

Output ceiling (0...255)

2

Gamma. Short integer from 10...999 representing 0.1...9.99. Applied to all image data.

Level record sets order

The first set of levels is the master set that applies to all of the composite channels (RGB) when in composite image mode.

The remaining sets apply to the active channels individually; set two applies to channel one, the set three to channel two, etc., up until set 25, which applies to channel 24.

Sets 28 and 29 are reserved and should be set to zeros.

Indexed color

The exception to the normal order is when the mode is Indexed:

The first set is a master set.

The next three sets are created for the Red, Green, and Blue portions of the image's color table, and they are applied to the first channel.

The remaining sets apply to any remaining alpha channels that are active: for instance, if channel two is active, set five applies to it; if channel three is active, set six applies to it, etc., up until channel 27, which applies to channel 24.

Sets 28 and 29 are reserved and should be set to zeros.

Single active channels

Photoshop handles single active channels in a special fashion. When saving the levels applied to a single channel, the settings are stored into the master set, at the beginning of the file. Similarly, when reading a levels file for application to a single active channel, the master levels are the ones that will be used on that channel. This allows easy application of a single file to both RGB and grayscale images.

Photoshop CS (8.0) Additional information

At the end of the Version 2 file is the following information:

Extra level record info marker 'Lvls'

Extra levels marker
Length

Description

4

= 'Lvls' for extra level information

2

Version ( = 3)

2

Count of total level record structures. Subtract the legacy number of level record structures, 29, to determine how many are remaining in the file for reading.

Variable

Additianol level records according to count. See Level record structure

Monitor Setup
This format has been superseded by ICC profiles. See ICC1v42_2006-05.pdf for details.

Monitor settings files are accessed in Photoshop's Color Settings dialog, via the Edit menu (load only) .

Monitor setup file types
OS

Filetype/extension

Mac OS

8BMS

Windows

.AMS


Monitor setup file format
Length

Description

2

Version ( = 2.)

2

Gamma. Short integer from 75...300 representing 0.75...3.00.

2*2

White point. Two short integers as CIE chromaticity coordinates: x,y . x = 0...10000 representing 0.0...1.0000. y = 1...10000 representing 0.0001...1.0000.

6*2

Phosphors. Three sets of two integers giving x,y coordinates of the red, green, and blue phosphors. x = 0...10000 representing 0.0...1.0000. y = 1...10000 representing 0.0001...1.0000. In the order red x , red y ; green x , green y ; blue x , blue y .

Replace Color/Color Range
Replace Color settings files are loaded and saved in the Color Range dialog (available via the Select menu).

Replace color/Color range file types
OS

Filetype/extension

Mac OS

8BXT

Windows

.AXT


Replace color/Color range file format
Length

Description

2

Version ( = 1)

2

Short integer indicating what space the color components are in. 7 = Lab color, 8 = grayscale. No other values are supported.

6

Component ranges. Six unsigned byte values representing the range of colors within which a pixel's color must fall to be considered selected for color replacement, or color range selecting. Described in See Component range structure.

2

Fuzziness. Short integer from 0...200 controlling how colors close to selected colors are affected.

6

Transform settings.

When used with Replace Color: Three short integers from -100...100. Described in See Replace color transform settings.

When used with Color Range: Writes zeros into the three short integers and ignores.


 
Component range structure
Length

Description

1

if Lab (color space = 7): low endpoint of L value

if grayscale (color space = 8): low endpoint of gray range

1

if Lab: high endpoint of L value

if grayscale: 0

1

if Lab: low endpoint of a chrominance value

if grayscale: 0

1

if Lab: high endpoint of a chrominance value

if grayscale: 0

1

if Lab: low endpoint of b chrominance value

if grayscale: low endpoint of gray range

1

if Lab: high endpoint of b chrominance value

if grayscale: high endpoint of gray range


Replace color transform settings
Length

Description

2

Hue change. Short integer from -100...100.

2

Saturation change. Short integer from -100...100.

2

Lightness change Short integer from -100...100. .

Selective Color
Selective Color settings files are loaded and saved in Photoshop's Selective Color dialog.

Selective color file types
OS

Filetype/extension

Mac OS

8BSV

Windows

.ASV


Selective color file format
Length

Description

2

Version ( = 1)

2

Correction method.. 0 = Apply color correction in relative mode; 1 = Apply color correction in absolute mode.

80

Ten eight-byte plate correction records, described in See Plate correction structure.

The first record is ignored by Photoshop and is reserved for future use. It should be set to all zeroes.

The rest of the records apply to specific areas of colors or lightness values in the image, in the following order: reds, yellows, greens, cyans, blues, magentas, whites, neutrals, blacks.


Plate correction structure
Length

Description

2

Amount of cyan correction. Short integer from -100...100.

2

Amount of magenta correction. Short integer from -100...100.

2

Amount of yellow correction. Short integer from -100...100.

2

Amount of black correction. Short integer from -100...100.

Separation Tables
This format has been superseded by ICC profiles. See ICC1v42_2006-05.pdf for details.

Separation Table files are accessed in the Separation Tables dialog (load only) .

Separation table file types
OS

Filetype/extension

Mac OS

8BST

Windows

.AST

Format:

If the size of the file is 33 * 33 * 33 * 4 , then the file consists only of a
Lab->CMYK table as currently documented.

If the size of the file is ( 33 * 33 * 33 + 256 ) * 3 , then the file consists only of a CMYK->Lab table as currently documented.

Otherwise, the file has the format listed in See Separation table file format.


Separation table file format
Length

Description

2

Version ( = 300)

1

Boolean. True if contains Lab->CMYK table.

1

Boolean. True if contains CMYK->Lab table.

33*33*33*4

If file contains Lab->CMYK table, this section contains CMYK colors for 33*33*33 Lab colors. The CMYK colors are written in interleaved order, one byte each ink. 0 = 100%, 255 = 0%. See See Generating Lab source colors below.

(33*33*33 +256)*3

If file contains CMYK->Lab table, this section contains Lab colors for 33*33*33+256 CMYK colors. The Lab colors are written in interleaved order, one byte per component. See See Generating CMYK source colors below.

1

Boolean. True if gamut table follows.

1

If entry above is false , this byte will not be present.

If true, this byte should be set to 1 for compatibility.

(((33*33*33L)+7)>>3) if gamut table present, zero otherwise

Gamut table, if present. The gamut table is a bit table indexed in the same way as the Lab->CMYK table with the high bit of the first byte at index 0. See See Testing for bits in the gamut table below.

Generating Lab source colors

The Lab colors that are the source colors can be generated from the Lab->CMYK table with the following routine:

for (i = 0; i < 33; i++)

 for (j = 0; j < 33; j++)

  for (n = 0; n < 33; n++)

  {

   L = Min (i * 8, 255);

   a = Min (j * 8, 255);

   b = Min (n * 8, 255);

  }

Generating CMYK source colors

The CMYK colors that are the source colors can be generated from the CMYK->Lab table with the following routine:

for (i = 0; i < 33; i++)

 for (j = 0; j < 33; j++)

  for (n = 0; n < 33; n++)

  {

   c = Min (i * 8, 255);

   m = Min (j * 8, 255);

   y = Min (n * 8, 255);

   k = 255;

  }

for (i = 0; i < 256; i++)

{

 c = 255;

 m = 255;

 y = 255;

 k = i;

}

Testing for bits in the gamut table

To test the bit at bitIndex , use table:

([bitIndex >> 3] & (0x0080 >> (bitIndex & 0x07))) != 0.

bitIndex itself is calculated in the same way you would calculate an index into the Lab->CMYK table.

A result of 1 indicates that the color is in gamut and 0 indicates that it is out of gamut.

Transfer Function
在 Photoshop 的“双色调曲线”对话框中，可以通过“双色调选项”和“传递函数”对话框访问（仅加载）传递函数设置文件（在 Photoshop 7 中可通过“编辑”>“带预览的打印”访问，在早期版本中可通过“页面设置”或“打印选项”访问）。传递函数文件也可以加载到 Photoshop 的任何曲线对话框中，例如“曲线颜色调整”对话框。

传输函数文件类型
你

文件类型/扩展名

Mac OS

8BTF

视窗

.ATF


传输函数文件格式
长度

描述

2

版本（= 4）

112 (= 28*4)

四个传递函数，详见“传递函数结构”。

该文件始终包含四个函数。例如，在编写灰度图像的打印传输函数时，Photoshop 会写入用户界面中指定的单个传输函数的四个副本。


传递函数结构
长度

描述

26

曲线。一个包含 13 个短整型的数组，取值范围为 0 到 1000，分别代表 0.0 到 100.0。除第一个和最后一个值外，其余值均可为 -1，表示曲线上没有点。最后一个板之后的任何曲线都应等于零曲线。零传递曲线如下所示：0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 1000。

2

布尔值。0 = 沿用打印机的供墨曲线；1 = 覆盖打印机的默认转印曲线。

 