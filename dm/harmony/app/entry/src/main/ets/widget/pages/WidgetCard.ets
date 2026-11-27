const storage = new LocalStorage();

class HeroRow {
  name: string = ''; cls: string = ''; initial: string = ''; img: string = ''; classIndex: number = -1; level: number = 0; gold: string = ''; time: string = '';
}

// 暗金风格卡片（数据由 EntryFormAbility.buildCard 给），以职业头像为主。2×2：最近一位英雄的大头像；2×4：最多三位并排；4×4：最多六位（两排）加「继续游戏」。
// Dark-gold card (data from EntryFormAbility.buildCard), built around class portraits. 2×2: the latest hero, large; 2×4: up to three side by side;
// 4×4: up to six (two rows) plus "continue".
@Entry(storage)
@Component
struct WidgetCard {
  @LocalStorageProp('dim') dim: number = 2;
  @LocalStorageProp('has') has: boolean = false;
  @LocalStorageProp('count') count: number = 0;
  @LocalStorageProp('heroes') heroes: HeroRow[] = [];
  @LocalStorageProp('title') title: string = 'Diablo II';
  @LocalStorageProp('empty') empty: string = '';
  @LocalStorageProp('levelWord') levelWord: string = 'Lv';
  @LocalStorageProp('goldWord') goldWord: string = 'Gold';
  @LocalStorageProp('countText') countText: string = '';
  @LocalStorageProp('resume') resume: string = '';

  // 职业头像（原版 HIREABLES 图标，自带金框）做卡片主体；没有头像时退回职业首字的圆章。
  // The class portrait (the game's HIREABLES icon, with its own gold frame) is the card's main element; without it, a round badge with the class initial.
  @Builder
  badge(hero: HeroRow, size: number) {
    if (hero.img) {
      Image(hero.img).width(size).height(size).objectFit(ImageFit.Contain).interpolation(ImageInterpolation.High)
        .borderRadius(size * 0.08).shadow({ radius: 8, color: '#99000000', offsetY: 2 })
    } else {
      Stack() { Text(hero.initial).fontSize(size * 0.46).fontWeight(FontWeight.Bold).fontColor('#F2D38A') }
        .width(size).height(size).backgroundColor('#1B140D').borderRadius(size / 2)
        .border({ width: 1.5, color: '#9C7A40', radius: size / 2 })
    }
  }

  lvText(h: HeroRow): string { return this.levelWord === 'Lv' ? `Lv ${h.level}` : `${h.level} ${this.levelWord}`; }

  // 一位英雄的竖排小块：头像、名字、等级 · 职业 | One hero as a vertical tile: portrait, name, level · class
  @Builder
  tile(h: HeroRow, size: number) {
    Column({ space: 3 }) {
      this.badge(h, size)
      Text(h.name).fontSize(size >= 60 ? 13 : 12).fontWeight(FontWeight.Bold).fontColor('#E8C67A').maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis }).textAlign(TextAlign.Center).width('100%')
      Text(`${this.lvText(h)} · ${h.cls}`).fontSize(10).fontColor('#D9C9A6').maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis }).textAlign(TextAlign.Center).width('100%')
    }.layoutWeight(1).alignItems(HorizontalAlign.Center)
  }

  build() {
    Column() {
      if (!this.has) {
        Column({ space: 6 }) {
          Text(this.title).fontSize(11).fontColor('#B08A4A')
          Text('D2R Mobile').fontSize(this.dim === 2 ? 18 : 22).fontWeight(FontWeight.Bold).fontColor('#E8C67A')
          Blank()
          Text(this.empty).fontSize(12).fontColor('#BFAE8A')
        }.width('100%').height('100%').alignItems(HorizontalAlign.Start)
      } else if (this.dim === 2) {
        Column({ space: 4 }) {
          this.badge(this.heroes[0], 78)
          Text(this.heroes[0].name).fontSize(15).fontWeight(FontWeight.Bold).fontColor('#E8C67A').maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis }).textAlign(TextAlign.Center).width('100%')
          Text(`${this.lvText(this.heroes[0])} · ${this.heroes[0].cls}`).fontSize(11).fontColor('#D9C9A6').maxLines(1)
          Text(this.count > 1 ? this.countText : this.heroes[0].time).fontSize(9).fontColor('#7D7466').maxLines(1)
        }.width('100%').height('100%').justifyContent(FlexAlign.Center).alignItems(HorizontalAlign.Center)
      } else {
        Column({ space: this.dim === 8 ? 10 : 6 }) {
          Row() {
            Text(this.title).fontSize(11).fontColor('#B08A4A')
            Blank()
            Text(this.countText).fontSize(9).fontColor('#7D7466')
          }.width('100%')
          if (this.dim === 4) {
            Row({ space: 6 }) {
              ForEach(this.heroes, (h: HeroRow) => { this.tile(h, 56) }, (h: HeroRow) => `${h.name}-${h.level}-${h.classIndex}`)
            }.width('100%').layoutWeight(1).alignItems(VerticalAlign.Center)
          } else {
            Row({ space: 6 }) {
              ForEach(this.heroes.slice(0, 3), (h: HeroRow) => { this.tile(h, 66) }, (h: HeroRow) => `${h.name}-${h.level}-${h.classIndex}`)
            }.width('100%').alignItems(VerticalAlign.Top)
            if (this.heroes.length > 3) {
              Row({ space: 6 }) {
                ForEach(this.heroes.slice(3, 6), (h: HeroRow) => { this.tile(h, 66) }, (h: HeroRow) => `${h.name}-${h.level}-${h.classIndex}`)
              }.width('100%').alignItems(VerticalAlign.Top)
            }
            Blank()
            Text(this.resume).fontSize(14).fontWeight(FontWeight.Bold).fontColor('#1B140D').width('100%').height(36).textAlign(TextAlign.Center)
              .borderRadius(18).linearGradient({ angle: 90, colors: [['#8C6A2C', 0], ['#E8C67A', 1]] })
          }
        }.width('100%').height('100%').alignItems(HorizontalAlign.Start)
      }
    }
    .width('100%').height('100%').padding(12)
    .linearGradient({ angle: 160, colors: [['#2A1E14', 0], ['#0E0B08', 1]] })
    .border({ width: 1, color: '#4A3824', radius: 16 })
    .onClick(() => { postCardAction(this, { action: 'router', abilityName: 'EntryAbility', params: {} }); })
  }
}
