const storage = new LocalStorage();

class HeroRow {
  name: string = ''; cls: string = ''; level: number = 0; pct: number = 0; bar: number = 0; gold: number = 0; img: string = ''; initial: string = '';
}

// Dark-gold Diablo card. 2x2: portrait, name, level / class, experience bar. 2x4: up to three heroes with bars and gold.
@Entry(storage)
@Component
struct WidgetCard {
  @LocalStorageProp('has') has: boolean = false;
  @LocalStorageProp('count') count: number = 0;
  @LocalStorageProp('heroes') heroes: HeroRow[] = [];
  @LocalStorageProp('name') name: string = '小菠萝2';
  @LocalStorageProp('cls') cls: string = '';
  @LocalStorageProp('level') level: number = 0;
  @LocalStorageProp('pct') pct: number = 0;
  @LocalStorageProp('bar') bar: number = 0;
  @LocalStorageProp('gold') gold: number = 0;
  @LocalStorageProp('img') img: string = '';
  @LocalStorageProp('initial') initial: string = '';
  @LocalStorageProp('time') time: string = '';
  @LocalStorageProp('wide') wide: boolean = false;

  @Builder
  portrait(img: string, initial: string, size: number) {
    if (img) {
      Image(img).width(size).height(size).objectFit(ImageFit.Contain).borderRadius(4)
    } else {
      Text(initial || '菠').width(size).height(size).textAlign(TextAlign.Center).fontSize(size * 0.45).fontColor('#E8C67A')
        .backgroundColor('#1B140D').border({ width: 1.5, color: '#8A6A3A', radius: 4 })
    }
  }

  @Builder
  expBar(permille: number, height: number) {
    Stack({ alignContent: Alignment.Start }) {
      Row().width('100%').height(height).backgroundColor('#16120E').border({ width: 1, color: '#46392A', radius: height / 2 })
      Row().width(`${Math.max(2, permille / 10)}%`).height(height).borderRadius(height / 2)
        .linearGradient({ angle: 90, colors: [['#8C6A2C', 0], ['#E8C67A', 1]] })
    }.width('100%').height(height)
  }

  build() {
    Column() {
      if (!this.has) {
        Column({ space: 6 }) {
          Text('暗黑破坏神 II').fontSize(11).fontColor('#B08A4A')
          Text('小菠萝2').fontSize(20).fontWeight(FontWeight.Bold).fontColor('#E8C67A')
          Blank()
          Text('还没有英雄 · 点开创建').fontSize(12).fontColor('#BFAE8A')
        }.width('100%').height('100%').alignItems(HorizontalAlign.Start)
      } else if (!this.wide) {
        // 2x2
        Column({ space: 6 }) {
          Row({ space: 8 }) {
            this.portrait(this.img, this.initial, 44)
            Column({ space: 2 }) {
              Text(this.name).fontSize(15).fontWeight(FontWeight.Bold).fontColor('#E8C67A').maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
              Text(`${this.level} 级 · ${this.cls}`).fontSize(11).fontColor('#D9C9A6')
            }.alignItems(HorizontalAlign.Start).layoutWeight(1)
          }.width('100%')
          Blank()
          Row() {
            Text('经验').fontSize(10).fontColor('#9C9384')
            Blank()
            Text(`${this.pct}%`).fontSize(10).fontColor('#E8C67A')
          }.width('100%')
          this.expBar(this.bar, 7)
          Text(this.count > 1 ? `共 ${this.count} 位英雄` : this.time).fontSize(9).fontColor('#7D7466').maxLines(1)
        }.width('100%').height('100%').alignItems(HorizontalAlign.Start)
      } else {
        // 2x4: hero list
        Column({ space: 6 }) {
          Row() {
            Text('暗黑破坏神 II · 小菠萝2').fontSize(11).fontColor('#B08A4A')
            Blank()
            Text(this.time).fontSize(9).fontColor('#7D7466')
          }.width('100%')
          ForEach(this.heroes, (h: HeroRow) => {
            Row({ space: 8 }) {
              this.portrait(h.img, h.initial, 34)
              Column({ space: 3 }) {
                Row() {
                  Text(h.name).fontSize(13).fontWeight(FontWeight.Medium).fontColor('#E8C67A').maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis }).layoutWeight(1)
                  Text(`Lv ${h.level} · ${h.cls}`).fontSize(10).fontColor('#D9C9A6')
                }.width('100%')
                this.expBar(h.bar, 5)
                Row() {
                  Text(`经验 ${h.pct}%`).fontSize(9).fontColor('#9C9384')
                  Blank()
                  Text(`金币 ${h.gold}`).fontSize(9).fontColor('#C9A85A')
                }.width('100%')
              }.layoutWeight(1).alignItems(HorizontalAlign.Start)
            }.width('100%')
          }, (h: HeroRow) => `${h.name}-${h.level}-${h.bar}`)
        }.width('100%').height('100%').alignItems(HorizontalAlign.Start)
      }
    }
    .width('100%').height('100%').padding(12)
    .linearGradient({ angle: 160, colors: [['#2A1E14', 0], ['#0E0B08', 1]] })
    .border({ width: 1, color: '#4A3824', radius: 16 })
    .onClick(() => { postCardAction(this, { action: 'router', abilityName: 'EntryAbility', params: {} }); })
  }
}
