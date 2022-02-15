using System.ComponentModel.DataAnnotations;
using System.ComponentModel.DataAnnotations.Schema;

namespace Playerbot.Core.Models;

[Table("playerbot_tokens")]
public class PlayerbotToken
{
    [Column("accountid")] [Key] public int AccountId { get; set; }
    [Column("token")] public string Token { get; set; } = string.Empty;
    [Column("age")] public DateTime Age { get; set; }
}
